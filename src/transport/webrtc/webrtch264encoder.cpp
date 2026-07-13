#include "transport/webrtc/webrtch264encoder.h"

#include "common/latencytracelogger.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QString>

#include <api/make_ref_counted.h>
#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <modules/video_coding/codecs/h264/include/h264_globals.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace
{
	constexpr char kH264CodecName[] = "H264";
	constexpr char kH264ProfileLevelId[] = "profile-level-id";
	constexpr char kH264ConstrainedBaselineLevel31[] = "42e01f";
	constexpr char kH264LevelAsymmetryAllowed[] = "level-asymmetry-allowed";
	constexpr char kH264PacketizationMode[] = "packetization-mode";
	constexpr int kDefaultFps = 30;
	constexpr int kDefaultBitrateKbps = 3000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr int kVideoTraceFrameInterval = 30;
	constexpr int kH264NaluTypeMask = 0x1f;
	constexpr int kH264IdrNaluType = 5;
	constexpr int kH264SpsNaluType = 7;
	constexpr int kMaxPlayoutDelayMs = 40950;
	constexpr char kPlayoutDelayMaxMsEnvName[] = "WRC_PLAYOUT_DELAY_MAX_MS";

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

	static int clampFps(int nFps)
	{
		return std::clamp(nFps, 1, 60);
	}

	static int bitrateKbpsFromCodec(const webrtc::VideoCodec *pCodecSettings)
	{
		if (pCodecSettings == nullptr)
			return kDefaultBitrateKbps;

		if (pCodecSettings->startBitrate > 0)
			return static_cast<int>(pCodecSettings->startBitrate);

		if (pCodecSettings->maxBitrate > 0)
			return static_cast<int>(pCodecSettings->maxBitrate);

		return kDefaultBitrateKbps;
	}

	static void appendPacket(QByteArray *pDst, const QByteArray &packet)
	{
		if (pDst == nullptr || packet.isEmpty())
			return;

		pDst->append(packet);
	}
}

KWebRtcH264Encoder::KWebRtcH264Encoder()
{
}

KWebRtcH264Encoder::~KWebRtcH264Encoder()
{
	Release();
}

int KWebRtcH264Encoder::InitEncode(const webrtc::VideoCodec *pCodecSettings,
	const webrtc::VideoEncoder::Settings &)
{
	if (pCodecSettings == nullptr
		|| pCodecSettings->width < 2
		|| pCodecSettings->height < 2)
	{
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	m_nWidth = static_cast<int>(pCodecSettings->width) & ~1;
	m_nHeight = static_cast<int>(pCodecSettings->height) & ~1;
	m_nFps = clampFps(static_cast<int>(pCodecSettings->maxFramerate > 0
			? pCodecSettings->maxFramerate
			: kDefaultFps));
	m_nBitrateKbps = std::max(1, bitrateKbpsFromCodec(pCodecSettings));
	m_nEncodedFrameCount = 0;
	m_bNeedKeyFrame = true;
	m_playoutDelay.reset();
	const QString strPlayoutDelayValue = qEnvironmentVariable(kPlayoutDelayMaxMsEnvName).trimmed();
	if (!qEnvironmentVariableIsSet(kPlayoutDelayMaxMsEnvName) || strPlayoutDelayValue.isEmpty())
	{
		m_playoutDelay = webrtc::VideoPlayoutDelay::Minimal();
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("playout_delay_config"),
			QStringLiteral("minMs=0 maxMs=0 source=default"));
	}
	else if (strPlayoutDelayValue.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("playout_delay_config"),
			QStringLiteral("mode=webrtc_default source=environment"));
	}
	else
	{
		bool bValidDelay = false;
		const int nMaxPlayoutDelayMs = strPlayoutDelayValue.toInt(&bValidDelay);
		if (bValidDelay && nMaxPlayoutDelayMs >= 0 && nMaxPlayoutDelayMs <= kMaxPlayoutDelayMs)
		{
			m_playoutDelay.emplace(webrtc::TimeDelta::Zero(),
				webrtc::TimeDelta::Millis(nMaxPlayoutDelayMs));
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("playout_delay_config"),
				QStringLiteral("minMs=0 maxMs=%1").arg(nMaxPlayoutDelayMs));
		}
		else
		{
			m_playoutDelay = webrtc::VideoPlayoutDelay::Minimal();
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("playout_delay_config_invalid"),
				QStringLiteral("value=%1 fallbackMinMs=0 fallbackMaxMs=0")
					.arg(strPlayoutDelayValue));
		}
	}

	QString strError;
	const bool bOpen = m_encoder.openStream(m_nWidth,
		m_nHeight,
		m_nFps,
		m_nBitrateKbps,
		[](const QByteArray &)
		{
		},
		&strError);
	if (!bOpen)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encoder_init_failed"),
			QStringLiteral("error=%1").arg(strError));
		return WEBRTC_VIDEO_CODEC_ERROR;
	}

	if (!m_encoder.fallbackReason().isEmpty())
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encoder_fallback"),
			QStringLiteral("from=h264_mf to=libx264 error=%1").arg(m_encoder.fallbackReason()));
	}
	KLatencyTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("webrtc_codec_selected"),
		QStringLiteral("codec=H264 encoder=%1").arg(m_encoder.encoderName()));
	KLatencyTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("h264_encoder_init"),
		QStringLiteral("encoder=%1 width=%2 height=%3 fps=%4 bitrateKbps=%5 fallback=%6")
			.arg(m_encoder.encoderName())
			.arg(m_nWidth)
			.arg(m_nHeight)
			.arg(m_nFps)
			.arg(m_nBitrateKbps)
			.arg(m_encoder.fallbackReason().isEmpty() ? 0 : 1));
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *pCallback)
{
	m_pCallback = pCallback;
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Encoder::Release()
{
	m_encoder.setDataCallback(KH264Encoder::DataCallback());
	QString strIgnoredError;
	m_encoder.close(&strIgnoredError);
	m_bNeedKeyFrame = true;
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Encoder::Encode(const webrtc::VideoFrame &frame,
	const std::vector<webrtc::VideoFrameType> *pFrameTypes)
{
	if (m_pCallback == nullptr || !m_encoder.isOpen())
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

	webrtc::scoped_refptr<webrtc::I420BufferInterface> spI420 = frame.video_frame_buffer()->ToI420();
	if (!spI420)
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

	QByteArray encodedFrame;
	m_encoder.setDataCallback([&encodedFrame](const QByteArray &packet)
		{
			appendPacket(&encodedFrame, packet);
		});
	QString strError;

	const bool bForceKeyFrame = m_bNeedKeyFrame || shouldForceKeyFrame(pFrameTypes);
	const qint64 nEncodeStartMs = QDateTime::currentMSecsSinceEpoch();
	QElapsedTimer encodeTimer;
	encodeTimer.start();
	if (!m_encoder.encodeI420Frame(spI420->DataY(),
			spI420->StrideY(),
			spI420->DataU(),
			spI420->StrideU(),
			spI420->DataV(),
			spI420->StrideV(),
			spI420->width(),
			spI420->height(),
			frame.timestamp_us() / 1000,
			bForceKeyFrame,
			&strError))
	{
		m_encoder.setDataCallback(KH264Encoder::DataCallback());
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encode_failed"),
			QStringLiteral("encoder=%1 error=%2").arg(m_encoder.encoderName(), strError));
		return WEBRTC_VIDEO_CODEC_ERROR;
	}
	m_encoder.setDataCallback(KH264Encoder::DataCallback());
	const qint64 nEncodeFinishMs = QDateTime::currentMSecsSinceEpoch();

	if (encodedFrame.isEmpty())
		return WEBRTC_VIDEO_CODEC_OK;

	QByteArray annexBFrame = normalizeToAnnexB(encodedFrame);
	if (annexBFrame.isEmpty())
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encode_failed"),
			QStringLiteral("encoder=%1 error=normalize_annexb_failed bytes=%2")
				.arg(m_encoder.encoderName())
				.arg(encodedFrame.size()));
		return WEBRTC_VIDEO_CODEC_ERROR;
	}

	const bool bKeyFrame = isKeyFrame(annexBFrame);
	if (bKeyFrame)
	{
		const QByteArray annexBHeader = normalizeToAnnexB(m_encoder.codecHeaderData());
		if (!annexBHeader.isEmpty() && !annexBFrame.startsWith(annexBHeader))
			annexBFrame.prepend(annexBHeader);
	}

	if (bKeyFrame)
		m_bNeedKeyFrame = false;

	++m_nEncodedFrameCount;
	if (m_nEncodedFrameCount % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encode_end"),
			QStringLiteral("encoder=%1 frame=%2 costMs=%3 bytes=%4 keyframe=%5 bitrateKbps=%6")
				.arg(m_encoder.encoderName())
				.arg(m_nEncodedFrameCount)
				.arg(encodeTimer.elapsed())
				.arg(annexBFrame.size())
				.arg(bKeyFrame ? 1 : 0)
				.arg(m_nBitrateKbps));
	}

	return emitEncodedFrame(frame, annexBFrame, nEncodeStartMs, nEncodeFinishMs)
		? WEBRTC_VIDEO_CODEC_OK
		: WEBRTC_VIDEO_CODEC_ERROR;
}

void KWebRtcH264Encoder::SetRates(const webrtc::VideoEncoder::RateControlParameters &parameters)
{
	const int nBitrateKbps = static_cast<int>(parameters.bitrate.get_sum_bps() / kBitsPerKilobit);
	if (nBitrateKbps > 0)
	{
		m_nBitrateKbps = nBitrateKbps;
		m_encoder.setBitrateKbps(m_nBitrateKbps);
	}

	if (parameters.framerate_fps > 0.0)
		m_nFps = clampFps(static_cast<int>(std::round(parameters.framerate_fps)));
}

webrtc::VideoEncoder::EncoderInfo KWebRtcH264Encoder::GetEncoderInfo() const
{
	webrtc::VideoEncoder::EncoderInfo info;
	const QString strEncoderName = m_encoder.encoderName();
	info.implementation_name = strEncoderName.isEmpty()
		? "h264_mf_or_libx264"
		: strEncoderName.toStdString();
	info.is_hardware_accelerated = strEncoderName == QStringLiteral("h264_mf");
	info.supports_native_handle = false;
	info.requested_resolution_alignment = 2;
	info.apply_alignment_to_all_simulcast_layers = true;
	info.scaling_settings = webrtc::VideoEncoder::ScalingSettings::kOff;
	info.has_trusted_rate_controller = false;
	return info;
}

bool KWebRtcH264Encoder::shouldForceKeyFrame(const std::vector<webrtc::VideoFrameType> *pFrameTypes) const
{
	if (pFrameTypes == nullptr)
		return false;

	return std::find(pFrameTypes->begin(),
		pFrameTypes->end(),
		webrtc::VideoFrameType::kVideoFrameKey) != pFrameTypes->end();
}

bool KWebRtcH264Encoder::emitEncodedFrame(const webrtc::VideoFrame &frame,
	const QByteArray &encodedData,
	qint64 nEncodeStartMs,
	qint64 nEncodeFinishMs)
{
	if (m_pCallback == nullptr || encodedData.isEmpty())
		return false;

	webrtc::EncodedImage image;
	image._encodedWidth = static_cast<uint32_t>(m_nWidth);
	image._encodedHeight = static_cast<uint32_t>(m_nHeight);
	image.SetRtpTimestamp(frame.rtp_timestamp());
	image.ntp_time_ms_ = frame.ntp_time_ms();
	image.capture_time_ms_ = frame.timestamp_us() / 1000;
	image.SetColorSpace(frame.color_space());
	image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
			reinterpret_cast<const uint8_t *>(encodedData.constData()),
			static_cast<size_t>(encodedData.size())));

	const bool bKeyFrame = isKeyFrame(encodedData);
	image.set_frame_type(bKeyFrame
		? webrtc::VideoFrameType::kVideoFrameKey
		: webrtc::VideoFrameType::kVideoFrameDelta);
	image.SetSimulcastIndex(0);
	image.SetEncodeTime(nEncodeStartMs, nEncodeFinishMs);
	image.set_end_of_temporal_unit(true);
	image.SetPlayoutDelay(m_playoutDelay);

	webrtc::CodecSpecificInfo codecInfo;
	codecInfo.codecType = webrtc::kVideoCodecH264;
	codecInfo.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
	codecInfo.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
	codecInfo.codecSpecific.H264.base_layer_sync = false;
	codecInfo.codecSpecific.H264.idr_frame = bKeyFrame;

	const webrtc::EncodedImageCallback::Result result =
		m_pCallback->OnEncodedImage(image, &codecInfo);
	return result.error == webrtc::EncodedImageCallback::Result::OK;
}

QByteArray KWebRtcH264Encoder::normalizeToAnnexB(const QByteArray &encodedData)
{
	if (encodedData.isEmpty())
		return QByteArray();

	if (hasAnnexBStartCode(encodedData))
		return encodedData;

	QByteArray annexBData;
	if (convertLengthPrefixedToAnnexB(encodedData, &annexBData))
		return annexBData;
	if (convertAvccConfigToAnnexB(encodedData, &annexBData))
		return annexBData;

	return QByteArray();
}

bool KWebRtcH264Encoder::hasAnnexBStartCode(const QByteArray &encodedData)
{
	const auto *pData = reinterpret_cast<const unsigned char *>(encodedData.constData());
	const int nSize = encodedData.size();
	for (int i = 0; i + 3 < nSize; ++i)
	{
		if (pData[i] == 0 && pData[i + 1] == 0 && pData[i + 2] == 1)
			return true;
		if (i + 4 < nSize && pData[i] == 0 && pData[i + 1] == 0 && pData[i + 2] == 0 && pData[i + 3] == 1)
			return true;
	}

	return false;
}

bool KWebRtcH264Encoder::convertLengthPrefixedToAnnexB(const QByteArray &encodedData, QByteArray *pAnnexBData)
{
	if (pAnnexBData == nullptr)
		return false;

	const auto *pData = reinterpret_cast<const unsigned char *>(encodedData.constData());
	const int nSize = encodedData.size();
	int nOffset = 0;
	QByteArray annexBData;
	while (nOffset + 4 <= nSize)
	{
		const int nNalSize =
			(static_cast<int>(pData[nOffset]) << 24)
			| (static_cast<int>(pData[nOffset + 1]) << 16)
			| (static_cast<int>(pData[nOffset + 2]) << 8)
			| static_cast<int>(pData[nOffset + 3]);
		nOffset += 4;
		if (nNalSize <= 0 || nOffset + nNalSize > nSize)
			return false;

		static constexpr char kStartCode[] = { 0, 0, 0, 1 };
		annexBData.append(kStartCode, sizeof(kStartCode));
		annexBData.append(reinterpret_cast<const char *>(pData + nOffset), nNalSize);
		nOffset += nNalSize;
	}

	if (nOffset != nSize || annexBData.isEmpty())
		return false;

	*pAnnexBData = std::move(annexBData);
	return true;
}

bool KWebRtcH264Encoder::convertAvccConfigToAnnexB(const QByteArray &configData, QByteArray *pAnnexBData)
{
	if (pAnnexBData == nullptr || configData.size() < 7)
		return false;

	const auto *pData = reinterpret_cast<const unsigned char *>(configData.constData());
	if (pData[0] != 1)
		return false;

	int nOffset = 5;
	const int nSpsCount = pData[nOffset++] & 0x1f;
	QByteArray annexBData;
	static constexpr char kStartCode[] = { 0, 0, 0, 1 };
	for (int i = 0; i < nSpsCount; ++i)
	{
		if (nOffset + 2 > configData.size())
			return false;
		const int nSpsSize = (static_cast<int>(pData[nOffset]) << 8)
			| static_cast<int>(pData[nOffset + 1]);
		nOffset += 2;
		if (nSpsSize <= 0 || nOffset + nSpsSize > configData.size())
			return false;

		annexBData.append(kStartCode, sizeof(kStartCode));
		annexBData.append(reinterpret_cast<const char *>(pData + nOffset), nSpsSize);
		nOffset += nSpsSize;
	}

	if (nOffset >= configData.size())
		return false;

	const int nPpsCount = pData[nOffset++];
	for (int i = 0; i < nPpsCount; ++i)
	{
		if (nOffset + 2 > configData.size())
			return false;
		const int nPpsSize = (static_cast<int>(pData[nOffset]) << 8)
			| static_cast<int>(pData[nOffset + 1]);
		nOffset += 2;
		if (nPpsSize <= 0 || nOffset + nPpsSize > configData.size())
			return false;

		annexBData.append(kStartCode, sizeof(kStartCode));
		annexBData.append(reinterpret_cast<const char *>(pData + nOffset), nPpsSize);
		nOffset += nPpsSize;
	}

	if (annexBData.isEmpty())
		return false;

	*pAnnexBData = std::move(annexBData);
	return true;
}

bool KWebRtcH264Encoder::isKeyFrame(const QByteArray &encodedData)
{
	const auto *pData = reinterpret_cast<const unsigned char *>(encodedData.constData());
	const int nSize = encodedData.size();
	for (int i = 0; i + 4 < nSize; ++i)
	{
		int nStartCodeSize = 0;
		if (pData[i] == 0 && pData[i + 1] == 0 && pData[i + 2] == 1)
			nStartCodeSize = 3;
		else if (i + 4 < nSize && pData[i] == 0 && pData[i + 1] == 0 && pData[i + 2] == 0 && pData[i + 3] == 1)
			nStartCodeSize = 4;

		if (nStartCodeSize == 0)
			continue;

		const int nNalOffset = i + nStartCodeSize;
		if (nNalOffset >= nSize)
			continue;

		const int nNalType = pData[nNalOffset] & kH264NaluTypeMask;
		if (nNalType == kH264IdrNaluType || nNalType == kH264SpsNaluType)
			return true;
	}

	return false;
}

std::vector<webrtc::SdpVideoFormat> KWebRtcH264EncoderFactory::GetSupportedFormats() const
{
	return { h264SdpFormat() };
}

webrtc::VideoEncoderFactory::CodecSupport KWebRtcH264EncoderFactory::QueryCodecSupport(
	const webrtc::SdpVideoFormat &format,
	std::optional<std::string>,
	std::optional<webrtc::Resolution>) const
{
	return webrtc::VideoEncoderFactory::CodecSupport{
		.is_supported = isH264Format(format),
		.is_power_efficient = isH264Format(format)
	};
}

std::unique_ptr<webrtc::VideoEncoder> KWebRtcH264EncoderFactory::Create(const webrtc::Environment &,
	const webrtc::SdpVideoFormat &format)
{
	if (!isH264Format(format))
		return nullptr;

	return std::make_unique<KWebRtcH264Encoder>();
}

bool KWebRtcH264EncoderFactory::isH264Format(const webrtc::SdpVideoFormat &format)
{
	return equalsIgnoreCase(format.name, kH264CodecName);
}
