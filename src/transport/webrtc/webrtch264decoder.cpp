#include "transport/webrtc/webrtch264decoder.h"

#include "common/latencytracelogger.h"

#include <QtCore/QString>

#include <api/make_ref_counted.h>
#include <api/video/i420_buffer.h>
#include <modules/video_coding/include/video_error_codes.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <libyuv.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{
	constexpr char kH264CodecName[] = "H264";
	constexpr char kH264ProfileLevelId[] = "profile-level-id";
	constexpr char kH264ConstrainedBaselineLevel31[] = "42e01f";
	constexpr char kH264LevelAsymmetryAllowed[] = "level-asymmetry-allowed";
	constexpr char kH264PacketizationMode[] = "packetization-mode";
	constexpr int kYuvPlaneCount = 3;

	static bool equalsIgnoreCase(const std::string &left, const char *right)
	{
		const std::string strRight(right);
		if (left.size() != strRight.size())
			return false;

		for (size_t i = 0; i < left.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(left[i]))
				!= std::tolower(static_cast<unsigned char>(strRight[i])))
			{
				return false;
			}
		}

		return true;
	}

	static webrtc::SdpVideoFormat h264SdpFormat()
	{
		return webrtc::SdpVideoFormat(kH264CodecName,
			{
				{ kH264ProfileLevelId, kH264ConstrainedBaselineLevel31 },
				{ kH264LevelAsymmetryAllowed, "1" },
				{ kH264PacketizationMode, "1" }
			});
	}
}

KWebRtcH264Decoder::KWebRtcH264Decoder()
{
}

KWebRtcH264Decoder::~KWebRtcH264Decoder()
{
	Release();
}

bool KWebRtcH264Decoder::Configure(const Settings &)
{
	return openDecoder();
}

int32_t KWebRtcH264Decoder::Decode(const webrtc::EncodedImage &inputImage, int64_t)
{
	if (!openDecoder())
		return WEBRTC_VIDEO_CODEC_ERROR;
	if (inputImage.data() == nullptr || inputImage.size() == 0)
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

	const unsigned char *pInputData = inputImage.data();
	int nInputSize = static_cast<int>(inputImage.size());

	while (nInputSize > 0)
	{
		unsigned char *pPacketData = nullptr;
		int nPacketSize = 0;
		const int nParsedSize = av_parser_parse2(m_pParserContext,
			m_pCodecContext,
			&pPacketData,
			&nPacketSize,
			pInputData,
			nInputSize,
			AV_NOPTS_VALUE,
			AV_NOPTS_VALUE,
			0);
		if (nParsedSize < 0)
			return WEBRTC_VIDEO_CODEC_ERROR;

		pInputData += nParsedSize;
		nInputSize -= nParsedSize;

		if (nPacketSize > 0 && !sendPacket(pPacketData, nPacketSize, inputImage))
			return WEBRTC_VIDEO_CODEC_ERROR;
	}

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Decoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback *pCallback)
{
	m_pCallback = pCallback;
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Decoder::Release()
{
	releaseDecoder();
	return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo KWebRtcH264Decoder::GetDecoderInfo() const
{
	DecoderInfo info;
	info.implementation_name = "ffmpeg_h264";
	info.is_hardware_accelerated = false;
	return info;
}

bool KWebRtcH264Decoder::openDecoder()
{
	if (m_pCodecContext != nullptr && m_pParserContext != nullptr)
		return true;

	releaseDecoder();

	const AVCodec *pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (pCodec == nullptr)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("h264_decoder_init_failed"),
			QStringLiteral("error=decoder_not_found"));
		return false;
	}

	m_pParserContext = av_parser_init(AV_CODEC_ID_H264);
	m_pCodecContext = avcodec_alloc_context3(pCodec);
	m_pFrame = av_frame_alloc();
	m_pPacket = av_packet_alloc();
	if (m_pParserContext == nullptr
		|| m_pCodecContext == nullptr
		|| m_pFrame == nullptr
		|| m_pPacket == nullptr)
	{
		releaseDecoder();
		return false;
	}

	m_pCodecContext->thread_count = 1;
	const int nOpenResult = avcodec_open2(m_pCodecContext, pCodec, nullptr);
	if (nOpenResult < 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("h264_decoder_init_failed"),
			QStringLiteral("error=%1").arg(ffmpegErrorMessage(QStringLiteral("open"), nOpenResult)));
		releaseDecoder();
		return false;
	}

	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("h264_decoder_init"),
		QStringLiteral("decoder=ffmpeg_h264"));
	return true;
}

bool KWebRtcH264Decoder::sendPacket(const unsigned char *pData,
	int nSize,
	const webrtc::EncodedImage &inputImage)
{
	av_packet_unref(m_pPacket);
	const int nPacketResult = av_new_packet(m_pPacket, nSize);
	if (nPacketResult < 0)
		return false;

	std::memcpy(m_pPacket->data, pData, static_cast<size_t>(nSize));
	const int nSendResult = avcodec_send_packet(m_pCodecContext, m_pPacket);
	av_packet_unref(m_pPacket);
	if (nSendResult < 0)
		return false;

	return receiveFrames(inputImage);
}

bool KWebRtcH264Decoder::receiveFrames(const webrtc::EncodedImage &inputImage)
{
	while (true)
	{
		const int nReceiveResult = avcodec_receive_frame(m_pCodecContext, m_pFrame);
		if (nReceiveResult == AVERROR(EAGAIN) || nReceiveResult == AVERROR_EOF)
			return true;
		if (nReceiveResult < 0)
			return false;

		webrtc::scoped_refptr<webrtc::I420Buffer> spBuffer =
			webrtc::I420Buffer::Create(m_pFrame->width, m_pFrame->height);

		// FFmpeg's H.264 software decoder emits AV_PIX_FMT_YUV420P (= I420) by
		// default, so a same-format plane copy with stride alignment is enough;
		// sws_scale is only kept as a fallback for unexpected pixel formats.
		const bool bIsI420 = m_pFrame->format == AV_PIX_FMT_YUV420P;
		bool bConverted = false;
		if (bIsI420)
		{
			const int nCopyResult = libyuv::I420Copy(m_pFrame->data[0],
				m_pFrame->linesize[0],
				m_pFrame->data[1],
				m_pFrame->linesize[1],
				m_pFrame->data[2],
				m_pFrame->linesize[2],
				spBuffer->MutableDataY(),
				spBuffer->StrideY(),
				spBuffer->MutableDataU(),
				spBuffer->StrideU(),
				spBuffer->MutableDataV(),
				spBuffer->StrideV(),
				m_pFrame->width,
				m_pFrame->height);
			bConverted = nCopyResult == 0;
		}
		else
		{
			if (!ensureSwsContext(m_pFrame))
			{
				av_frame_unref(m_pFrame);
				return false;
			}

			unsigned char *pDstData[kYuvPlaneCount] = {
				spBuffer->MutableDataY(),
				spBuffer->MutableDataU(),
				spBuffer->MutableDataV()
			};
			int nDstStride[kYuvPlaneCount] = {
				spBuffer->StrideY(),
				spBuffer->StrideU(),
				spBuffer->StrideV()
			};

			const int nScaledRows = sws_scale(m_pSwsContext,
				m_pFrame->data,
				m_pFrame->linesize,
				0,
				m_pFrame->height,
				pDstData,
				nDstStride);
			bConverted = nScaledRows == m_pFrame->height;
		}

		av_frame_unref(m_pFrame);
		if (!bConverted)
			return false;

		webrtc::VideoFrame decodedFrame = webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(spBuffer)
			.set_timestamp_rtp(inputImage.RtpTimestamp())
			.set_timestamp_ms(inputImage.capture_time_ms_)
			.set_rotation(inputImage.rotation_)
			.build();

		if (m_pCallback != nullptr)
			m_pCallback->Decoded(decodedFrame);
	}
}

bool KWebRtcH264Decoder::ensureSwsContext(const AVFrame *pFrame)
{
	const int nFormat = static_cast<int>(pFrame->format);
	if (m_pSwsContext != nullptr
		&& m_nSwsWidth == pFrame->width
		&& m_nSwsHeight == pFrame->height
		&& m_nSwsFormat == nFormat)
	{
		return true;
	}

	if (m_pSwsContext != nullptr)
	{
		sws_freeContext(m_pSwsContext);
		m_pSwsContext = nullptr;
	}

	m_pSwsContext = sws_getContext(pFrame->width,
		pFrame->height,
		static_cast<AVPixelFormat>(pFrame->format),
		pFrame->width,
		pFrame->height,
		AV_PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR,
		nullptr,
		nullptr,
		nullptr);
	if (m_pSwsContext == nullptr)
		return false;

	m_nSwsWidth = pFrame->width;
	m_nSwsHeight = pFrame->height;
	m_nSwsFormat = nFormat;
	return true;
}

void KWebRtcH264Decoder::releaseDecoder()
{
	if (m_pSwsContext != nullptr)
	{
		sws_freeContext(m_pSwsContext);
		m_pSwsContext = nullptr;
	}
	if (m_pPacket != nullptr)
	{
		av_packet_free(&m_pPacket);
		m_pPacket = nullptr;
	}
	if (m_pFrame != nullptr)
	{
		av_frame_free(&m_pFrame);
		m_pFrame = nullptr;
	}
	if (m_pCodecContext != nullptr)
	{
		avcodec_free_context(&m_pCodecContext);
		m_pCodecContext = nullptr;
	}
	if (m_pParserContext != nullptr)
	{
		av_parser_close(m_pParserContext);
		m_pParserContext = nullptr;
	}
	m_nSwsWidth = 0;
	m_nSwsHeight = 0;
	m_nSwsFormat = -1;
}

QString KWebRtcH264Decoder::ffmpegErrorMessage(const QString &strPrefix, int nErrorCode)
{
	char szError[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(nErrorCode, szError, sizeof(szError));
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromLocal8Bit(szError));
}

std::vector<webrtc::SdpVideoFormat> KWebRtcH264DecoderFactory::GetSupportedFormats() const
{
	return { h264SdpFormat() };
}

webrtc::VideoDecoderFactory::CodecSupport KWebRtcH264DecoderFactory::QueryCodecSupport(
	const webrtc::SdpVideoFormat &format,
	bool,
	std::optional<webrtc::Resolution>) const
{
	return webrtc::VideoDecoderFactory::CodecSupport{
		.is_supported = isH264Format(format),
		.is_power_efficient = false
	};
}

std::unique_ptr<webrtc::VideoDecoder> KWebRtcH264DecoderFactory::Create(const webrtc::Environment &,
	const webrtc::SdpVideoFormat &format)
{
	if (!isH264Format(format))
		return nullptr;

	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("webrtc_decoder_selected"),
		QStringLiteral("codec=H264 decoder=ffmpeg_h264"));
	return std::make_unique<KWebRtcH264Decoder>();
}

bool KWebRtcH264DecoderFactory::isH264Format(const webrtc::SdpVideoFormat &format)
{
	return equalsIgnoreCase(format.name, kH264CodecName);
}
