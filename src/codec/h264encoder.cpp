#include "codec/h264encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <libyuv.h>

namespace
{
	constexpr std::int64_t kMinBitrate = 2'000'000;
	constexpr std::int64_t kMaxBitrate = 20'000'000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr int kMfScenarioDisplayRemoting = 1;
	constexpr int kMfRateControlLowDelayVbr = 4;
}

KH264Encoder::KH264Encoder()
{
}

KH264Encoder::~KH264Encoder()
{
	QString strIgnoredError;
	close(&strIgnoredError);
}

bool KH264Encoder::openStream(int nWidth,
	int nHeight,
	int nFps,
	const DataCallback &callback,
	QString *pErrorMessage)
{
	return openStream(nWidth, nHeight, nFps, 0, callback, pErrorMessage);
}

bool KH264Encoder::openStream(int nWidth,
	int nHeight,
	int nFps,
	int nBitrateKbps,
	const DataCallback &callback,
	QString *pErrorMessage)
{
	if (isOpen())
		close(nullptr);

	if (nWidth < 2 || nHeight < 2 || nFps <= 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 encoder open failed: invalid parameter");
		return false;
	}

	if (!callback)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 stream callback is empty");
		return false;
	}

	const AVCodec *pCodec = avcodec_find_encoder_by_name("h264_mf");
	if (pCodec == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("FFmpeg encoder h264_mf not found");
		return false;
	}

	m_pCodecContext = avcodec_alloc_context3(pCodec);
	if (m_pCodecContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create H.264 codec context failed");
		return false;
	}

	m_nEncodedWidth = nWidth & ~1;
	m_nEncodedHeight = nHeight & ~1;
	m_nFps = nFps;
	m_nBitrateKbps = nBitrateKbps;
	m_nFirstTimestampMs = 0;
	m_nLastPts = -1;
	m_dataCallback = callback;

	m_pCodecContext->codec_id = pCodec->id;
	m_pCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
	m_pCodecContext->width = m_nEncodedWidth;
	m_pCodecContext->height = m_nEncodedHeight;
	m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;
	m_pCodecContext->time_base = AVRational{ 1, m_nFps };
	m_pCodecContext->framerate = AVRational{ m_nFps, 1 };
	m_pCodecContext->gop_size = m_nFps * 2;
	m_pCodecContext->max_b_frames = 0;
	m_pCodecContext->thread_count = 1;
	const std::int64_t nRequestedBitrate = static_cast<std::int64_t>(m_nBitrateKbps) * kBitsPerKilobit;
	m_pCodecContext->bit_rate = nRequestedBitrate > 0
		? nRequestedBitrate
		: std::clamp(
			static_cast<std::int64_t>(m_nEncodedWidth) * m_nEncodedHeight * m_nFps / 4,
			kMinBitrate,
			kMaxBitrate);

	av_opt_set_int(m_pCodecContext->priv_data, "hw_encoding", 1, 0);
	av_opt_set_int(m_pCodecContext->priv_data, "scenario", kMfScenarioDisplayRemoting, 0);
	av_opt_set_int(m_pCodecContext->priv_data, "rate_control", kMfRateControlLowDelayVbr, 0);

	const int nOpenResult = avcodec_open2(m_pCodecContext, pCodec, nullptr);
	if (nOpenResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Open h264_mf failed"), nOpenResult);
		release();
		return false;
	}

	m_pFrame = av_frame_alloc();
	if (m_pFrame == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create video frame failed");
		release();
		return false;
	}

	m_pFrame->format = m_pCodecContext->pix_fmt;
	m_pFrame->width = m_pCodecContext->width;
	m_pFrame->height = m_pCodecContext->height;

	const int nFrameResult = av_frame_get_buffer(m_pFrame, 32);
	if (nFrameResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Allocate video frame buffer failed"), nFrameResult);
		release();
		return false;
	}

	m_pPacket = av_packet_alloc();
	if (m_pPacket == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create encoded packet failed");
		release();
		return false;
	}

	m_pSwsContext = sws_getContext(m_nEncodedWidth,
		m_nEncodedHeight,
		AV_PIX_FMT_BGRA,
		m_nEncodedWidth,
		m_nEncodedHeight,
		AV_PIX_FMT_NV12,
		SWS_FAST_BILINEAR,
		nullptr,
		nullptr,
		nullptr);
	if (m_pSwsContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create BGRA to NV12 converter failed");
		release();
		return false;
	}

	m_bOpen = true;
	return true;
}

bool KH264Encoder::encodeBgraFrame(const unsigned char *pBgraData,
	int nWidth,
	int nHeight,
	qint64 nTimestampMs,
	QString *pErrorMessage)
{
	if (!isOpen() || pBgraData == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 encoder is not open");
		return false;
	}

	if (nWidth < m_nEncodedWidth || nHeight < m_nEncodedHeight)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Input frame size is smaller than encoder size");
		return false;
	}

	const int nWritableResult = av_frame_make_writable(m_pFrame);
	if (nWritableResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Make video frame writable failed"), nWritableResult);
		return false;
	}

	const unsigned char *pSrcSlice[] = { pBgraData, nullptr, nullptr, nullptr };
	const int nSrcStride[] = { nWidth * 4, 0, 0, 0 };
	const int nScaleResult = sws_scale(m_pSwsContext,
		pSrcSlice,
		nSrcStride,
		0,
		m_nEncodedHeight,
		m_pFrame->data,
		m_pFrame->linesize);
	if (nScaleResult != m_nEncodedHeight)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Convert BGRA frame to NV12 failed");
		return false;
	}

	if (!prepareFrame(nTimestampMs, false, pErrorMessage))
		return false;

	const int nSendResult = avcodec_send_frame(m_pCodecContext, m_pFrame);
	if (nSendResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Send frame to H.264 encoder failed"), nSendResult);
		return false;
	}

	return writePacket(pErrorMessage);
}

bool KH264Encoder::encodeI420Frame(const unsigned char *pYData,
	int nStrideY,
	const unsigned char *pUData,
	int nStrideU,
	const unsigned char *pVData,
	int nStrideV,
	int nWidth,
	int nHeight,
	qint64 nTimestampMs,
	bool bForceKeyFrame,
	QString *pErrorMessage)
{
	if (!isOpen() || pYData == nullptr || pUData == nullptr || pVData == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 encoder is not open");
		return false;
	}

	if (nWidth < m_nEncodedWidth || nHeight < m_nEncodedHeight)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Input frame size is smaller than encoder size");
		return false;
	}

	const int nWritableResult = av_frame_make_writable(m_pFrame);
	if (nWritableResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Make video frame writable failed"), nWritableResult);
		return false;
	}

	const int nConvertResult = libyuv::I420ToNV12(pYData,
		nStrideY,
		pUData,
		nStrideU,
		pVData,
		nStrideV,
		m_pFrame->data[0],
		m_pFrame->linesize[0],
		m_pFrame->data[1],
		m_pFrame->linesize[1],
		m_nEncodedWidth,
		m_nEncodedHeight);
	if (nConvertResult != 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Convert I420 frame to NV12 failed");
		return false;
	}

	if (!prepareFrame(nTimestampMs, bForceKeyFrame, pErrorMessage))
		return false;

	const int nSendResult = avcodec_send_frame(m_pCodecContext, m_pFrame);
	if (nSendResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Send frame to H.264 encoder failed"), nSendResult);
		return false;
	}

	return writePacket(pErrorMessage);
}

void KH264Encoder::setBitrateKbps(int nBitrateKbps)
{
	m_nBitrateKbps = nBitrateKbps;
	if (m_pCodecContext != nullptr && nBitrateKbps > 0)
		m_pCodecContext->bit_rate = static_cast<std::int64_t>(nBitrateKbps) * kBitsPerKilobit;
}

void KH264Encoder::setDataCallback(const DataCallback &callback)
{
	m_dataCallback = callback;
}

bool KH264Encoder::close(QString *pErrorMessage)
{
	bool bResult = true;

	if (m_pCodecContext != nullptr)
	{
		const int nSendResult = avcodec_send_frame(m_pCodecContext, nullptr);
		if (nSendResult < 0 && nSendResult != AVERROR_EOF)
		{
			bResult = false;
			if (pErrorMessage != nullptr)
				*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Flush H.264 encoder failed"), nSendResult);
		}

		if (!writePacket(pErrorMessage))
			bResult = false;
	}

	release();
	return bResult;
}

bool KH264Encoder::isOpen() const
{
	return m_bOpen && m_pCodecContext != nullptr;
}

int KH264Encoder::encodedWidth() const
{
	return m_nEncodedWidth;
}

int KH264Encoder::encodedHeight() const
{
	return m_nEncodedHeight;
}

QByteArray KH264Encoder::codecHeaderData() const
{
	if (m_pCodecContext == nullptr
		|| m_pCodecContext->extradata == nullptr
		|| m_pCodecContext->extradata_size <= 0)
	{
		return QByteArray();
	}

	return QByteArray(reinterpret_cast<const char *>(m_pCodecContext->extradata),
		m_pCodecContext->extradata_size);
}

bool KH264Encoder::prepareFrame(qint64 nTimestampMs, bool bForceKeyFrame, QString *)
{
	if (m_nFirstTimestampMs <= 0)
		m_nFirstTimestampMs = nTimestampMs;

	std::int64_t nPts = static_cast<std::int64_t>(
		((nTimestampMs - m_nFirstTimestampMs) * static_cast<qint64>(m_nFps) + 500) / 1000);
	if (nPts <= m_nLastPts)
		nPts = m_nLastPts + 1;

	m_pFrame->pts = nPts;
	m_pFrame->pict_type = bForceKeyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
	m_nLastPts = nPts;
	return true;
}

bool KH264Encoder::writePacket(QString *pErrorMessage)
{
	while (m_pCodecContext != nullptr && m_pPacket != nullptr)
	{
		const int nResult = avcodec_receive_packet(m_pCodecContext, m_pPacket);
		if (nResult == AVERROR(EAGAIN) || nResult == AVERROR_EOF)
			return true;
		if (nResult < 0)
		{
			if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
				*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Receive H.264 packet failed"), nResult);
			return false;
		}

		if (m_dataCallback)
			m_dataCallback(QByteArray(reinterpret_cast<const char *>(m_pPacket->data), m_pPacket->size));
		av_packet_unref(m_pPacket);
	}

	return true;
}

void KH264Encoder::release()
{
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

	if (m_pSwsContext != nullptr)
	{
		sws_freeContext(m_pSwsContext);
		m_pSwsContext = nullptr;
	}

	if (m_pCodecContext != nullptr)
	{
		avcodec_free_context(&m_pCodecContext);
		m_pCodecContext = nullptr;
	}

	m_dataCallback = DataCallback();
	m_nEncodedWidth = 0;
	m_nEncodedHeight = 0;
	m_nBitrateKbps = 0;
	m_nFirstTimestampMs = 0;
	m_nLastPts = -1;
	m_bOpen = false;
}

QString KH264Encoder::ffmpegErrorMessage(const QString &strPrefix, int nErrorCode)
{
	char szBuffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(nErrorCode, szBuffer, sizeof(szBuffer));
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromLocal8Bit(szBuffer));
}
