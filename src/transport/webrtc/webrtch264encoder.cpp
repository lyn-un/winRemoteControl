#include "transport/webrtc/webrtch264encoder.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "transport/webrtc/encoderdiagnostics.h"
#include "transport/webrtc/webrtcnativeframebuffer.h"
#include "capture/d3d11bgraframe.h"
#include <QtCore/QScopeGuard>
#include "core/protocol/protocolconstraints.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtCore/QDebug>

#include <api/make_ref_counted.h>
#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <modules/video_coding/codecs/h264/include/h264_globals.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr int kDefaultFps = 30;
	constexpr int kDefaultBitrateKbps = 3000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr int kVideoTraceFrameInterval = 30;
	constexpr quint64 kRateTargetTraceInterval = 30;
	constexpr int kH264NaluTypeMask = 0x1f;
	constexpr int kH264IdrNaluType = 5;
	constexpr int kH264SpsNaluType = 7;
	constexpr int kMaxPlayoutDelayMs = 40950;
	constexpr char kPlayoutDelayMaxMsEnvName[] = "WRC_PLAYOUT_DELAY_MAX_MS";


	static int clampFps(int nFps)
	{
		return std::clamp(nFps,
			KProtocolConstraints::kMinimumStreamFps,
			KProtocolConstraints::kMaximumStreamFps);
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

}

KWebRtcH264Encoder::KWebRtcH264Encoder(KVideoEncoderPreference preference)
	: KWebRtcH264Encoder(preference, KH264LevelPolicy::kMaximumLevel)
{
}

KWebRtcH264Encoder::KWebRtcH264Encoder(KVideoEncoderPreference preference, int nNegotiatedLevel)
	: KWebRtcH264Encoder(preference, nNegotiatedLevel, nullptr)
{
}

KWebRtcH264Encoder::KWebRtcH264Encoder(KVideoEncoderPreference preference, int nNegotiatedLevel,
	std::shared_ptr<KVideoFramePathState> spFramePath)
	: m_encoder(preference)
	, m_spFramePath(std::move(spFramePath))
	, m_nNegotiatedLevel(nNegotiatedLevel)
	, m_spsGuard(nNegotiatedLevel)
{
}

KWebRtcH264Encoder::~KWebRtcH264Encoder()
{
	Release();
}

int KWebRtcH264Encoder::InitEncode(const webrtc::VideoCodec *pCodecSettings,
	const webrtc::VideoEncoder::Settings &)
{
	m_spDiagnostics.reset();
	if (KSessionTraceLogger::isEnabled())
		m_spDiagnostics = std::make_unique<KEncoderDiagnostics>();
	if (pCodecSettings == nullptr
		|| pCodecSettings->width < 2
		|| pCodecSettings->height < 2)
	{
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	m_nWidth = static_cast<int>(pCodecSettings->width) & ~1;
	m_nHeight = static_cast<int>(pCodecSettings->height) & ~1;
	m_nConfiguredFps = clampFps(static_cast<int>(pCodecSettings->maxFramerate > 0
			? pCodecSettings->maxFramerate
			: kDefaultFps));
	m_nTargetFps = m_nConfiguredFps;
	m_nBitrateKbps = std::max(1, bitrateKbpsFromCodec(pCodecSettings));
	m_nEncodedFrameCount = 0;
	m_bGpuAttempted = false;
	m_nGpuPoolDrops = 0;
	m_pendingFrames.clear();
	m_nNoOutputCount = 0;
	m_nRateTargetChangeCount = 0;
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
	m_encoder.close(nullptr);
	m_spsGuard.reset();
	m_bLevelError = false;
	const std::int64_t nPeakBps = pCodecSettings->maxBitrate > 0
		? static_cast<std::int64_t>(pCodecSettings->maxBitrate) * 1000
		: std::min<std::int64_t>(KProtocolConstraints::kMaximumStreamBitrateKbps * 1000LL,
			KH264LevelPolicy::maximumBitrate(m_nNegotiatedLevel));
	m_nEncodingLevel = KH264LevelPolicy::minimumLevel(m_nWidth, m_nHeight, m_nConfiguredFps,
		std::max(nPeakBps, m_nBitrateKbps * 1000LL), std::max(nPeakBps, m_nBitrateKbps * 1000LL) / 2, 1);
	if (!m_nEncodingLevel || m_nEncodingLevel > m_nNegotiatedLevel
		|| !m_encoder.setLevelLimit(m_nEncodingLevel))
	{
		qWarning() << "H.264 configuration cannot fit negotiated level" << m_nNegotiatedLevel
			<< m_nWidth << m_nHeight << m_nConfiguredFps << nPeakBps;
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}
	KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_level_config"),
		QStringLiteral("negotiatedLevel=%1 encodingLevel=%2 peakBudgetBps=%3")
			.arg(m_nNegotiatedLevel).arg(m_nEncodingLevel).arg(nPeakBps));
	const bool bOpen = m_encoder.openStream(m_nWidth,
		m_nHeight,
		m_nConfiguredFps,
		m_nBitrateKbps,
		[](const QByteArray &)
		{
		},
		&strError);
	if (!bOpen)
	{
		if (m_spDiagnostics)
			m_spDiagnostics->error(QStringLiteral("reason=init_failed detail=%1").arg(strError));
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encoder_init_failed"),
			QStringLiteral("error=%1").arg(strError));
		return WEBRTC_VIDEO_CODEC_ERROR;
	}

	if (!m_encoder.fallbackReason().isEmpty())
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encoder_fallback"),
			QStringLiteral("to=%1 error=%2").arg(m_encoder.encoderName(), m_encoder.fallbackReason()));
	}
	KLatencyTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("webrtc_codec_selected"),
		QStringLiteral("codec=H264 encoder=%1").arg(m_encoder.encoderName()));
	KLatencyTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("h264_encoder_init"),
		QStringLiteral("encoder=%1 width=%2 height=%3 fps=%4 requestedBitrateKbps=%5 fallback=%6")
			.arg(m_encoder.encoderName())
			.arg(m_nWidth)
			.arg(m_nHeight)
			.arg(m_nConfiguredFps)
			.arg(m_nBitrateKbps)
			.arg(m_encoder.fallbackReason().isEmpty() ? 0 : 1));
	if (m_spDiagnostics)
	{
		m_spDiagnostics->rates(QStringLiteral("encoder=%1 width=%2 height=%3 configuredFps=%4 initialKbps=%5")
			.arg(m_encoder.encoderName()).arg(m_nWidth).arg(m_nHeight).arg(m_nConfiguredFps).arg(m_nBitrateKbps));
		m_spDiagnostics->phase("idle");
	}
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *pCallback)
{
	m_pCallback = pCallback;
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Encoder::Release()
{
	if (m_spDiagnostics)
		m_spDiagnostics->phase("closing");
	m_encoder.setDataCallback(KH264Encoder::DataCallback());
	m_encoder.setPacketCallback(KH264Encoder::PacketCallback());
	m_pendingFrames.clear();
	QString strIgnoredError;
	m_encoder.close(&strIgnoredError);
	m_spsGuard.reset();
	m_bNeedKeyFrame = true;
	m_spDiagnostics.reset();
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t KWebRtcH264Encoder::Encode(const webrtc::VideoFrame &frame,
	const std::vector<webrtc::VideoFrameType> *pFrameTypes)
{
	bool bDiagnosticSuccess = false;
	if (m_spDiagnostics)
		m_spDiagnostics->begin();
	const auto diagnosticExit = qScopeGuard([this, &bDiagnosticSuccess]()
		{ if (m_spDiagnostics) m_spDiagnostics->finish(bDiagnosticSuccess); });
	if (m_bLevelError)
	{
		if (m_spDiagnostics)
			m_spDiagnostics->error(QStringLiteral("reason=latched_level_error"));
		return WEBRTC_VIDEO_CODEC_ERROR;
	}
	if (m_pCallback == nullptr || !m_encoder.isOpen())
	{
		if (m_spDiagnostics)
			m_spDiagnostics->error(QStringLiteral("reason=uninitialized callback=%1 open=%2")
				.arg(m_pCallback != nullptr).arg(m_encoder.isOpen()));
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	std::shared_ptr<IKNativeVideoFrame> spNative;
	if (frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative)
	{
		// This encoder's native frames originate exclusively in KWebRtcVideoSource.
		spNative = static_cast<KWebRtcNativeFrameBuffer *>(frame.video_frame_buffer().get())->nativeFrame();
		if (!spNative || !m_spFramePath || spNative->nativeType() != IKNativeVideoFrame::D3D11BgraNativeType
			|| spNative->generation() != m_spFramePath->snapshot().nGeneration)
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		if (!m_bGpuAttempted && m_spFramePath->snapshot().decision != KVideoFramePathState::Cpu)
		{
			m_bGpuAttempted = true;
			QString strGpuError;
			const bool bGpu = m_encoder.enableGpuFrames(*static_cast<KD3D11BgraFrame *>(spNative.get()), &strGpuError);
			if (bGpu)
				m_spFramePath->selectGpu(spNative->generation());
			else
				m_spFramePath->fallback(spNative->generation(), strGpuError);
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("video_frame_path"),
				QStringLiteral("requested=%1 actual=%2 encoder=%3 captureLuid=%4 reason=%5")
					.arg(QString::fromLatin1(VideoFramePathName(m_spFramePath->mode())))
					.arg(bGpu ? QStringLiteral("gpu") : (m_spFramePath->snapshot().decision == KVideoFramePathState::Failed
						? QStringLiteral("failed") : QStringLiteral("cpu")))
					.arg(m_encoder.encoderName()).arg(spNative->adapterLuid()).arg(strGpuError));
		}
		if (m_spFramePath->snapshot().decision == KVideoFramePathState::Failed)
			return WEBRTC_VIDEO_CODEC_ERROR;
	}
	const bool bGpuFrame = spNative && m_encoder.usesGpuFrames();
	webrtc::scoped_refptr<webrtc::I420BufferInterface> spI420;
	if (!bGpuFrame)
		spI420 = frame.video_frame_buffer()->ToI420();
	if (!bGpuFrame && !spI420)
	{
		if (m_spDiagnostics)
			m_spDiagnostics->error(QStringLiteral("reason=to_i420_failed"));
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	if (m_pendingFrames.size() >= 64)
	{
		if (m_spDiagnostics)
			m_spDiagnostics->error(QStringLiteral("reason=pending_metadata_limit"));
		KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_encode_failed"),
			QStringLiteral("error=pending_metadata_limit pending=64"));
		m_pendingFrames.clear();
		return WEBRTC_VIDEO_CODEC_ERROR;
	}
	std::map<std::int64_t, QByteArray> packets;
	m_encoder.setPacketCallback([&packets](const QByteArray &packet, std::int64_t nPts)
		{
			packets[nPts].append(packet);
		});
	QString strError;

	const bool bForceKeyFrame = m_bNeedKeyFrame || shouldForceKeyFrame(pFrameTypes);
	const qint64 nEncodeStartMs = QDateTime::currentMSecsSinceEpoch();
	QElapsedTimer encodeTimer;
	encodeTimer.start();
	if (m_spDiagnostics)
		m_spDiagnostics->phase(bGpuFrame ? "gpu_convert_submit" : "ffmpeg_encode");
	bool bEncodeOk = false;
	if (bGpuFrame)
	{
		const auto result = m_encoder.encodeGpuFrame(*static_cast<KD3D11BgraFrame *>(spNative.get()),
			frame.timestamp_us() / 1000, bForceKeyFrame, &strError);
		if (result == KH264Encoder::BusyGpuEncodeResult)
		{
			bDiagnosticSuccess = true;
			m_encoder.setPacketCallback(KH264Encoder::PacketCallback());
			++m_nGpuPoolDrops;
			if (m_nGpuPoolDrops == 1 || m_nGpuPoolDrops % 30 == 0)
				KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("gpu_encode_pool_drop"),
					QStringLiteral("dropped=%1").arg(m_nGpuPoolDrops));
			return WEBRTC_VIDEO_CODEC_OK;
		}
		bEncodeOk = result == KH264Encoder::SubmittedGpuEncodeResult;
	}
	else
		bEncodeOk = m_encoder.encodeI420Frame(spI420->DataY(),
			spI420->StrideY(),
			spI420->DataU(),
			spI420->StrideU(),
			spI420->DataV(),
			spI420->StrideV(),
			spI420->width(),
			spI420->height(),
			frame.timestamp_us() / 1000,
			bForceKeyFrame,
			&strError);
	if (!bEncodeOk)
	{
		if (bGpuFrame && m_spFramePath)
			m_spFramePath->fail(spNative->generation(), strError);
		if (m_spDiagnostics)
			m_spDiagnostics->error(QStringLiteral("reason=encode_failed detail=%1").arg(strError));
		m_encoder.setPacketCallback(KH264Encoder::PacketCallback());
		m_pendingFrames.clear();
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_encode_failed"),
			QStringLiteral("encoder=%1 error=%2").arg(m_encoder.encoderName(), strError));
		return WEBRTC_VIDEO_CODEC_ERROR;
	}
	m_encoder.setPacketCallback(KH264Encoder::PacketCallback());
	const qint64 nEncodeFinishMs = QDateTime::currentMSecsSinceEpoch();
	if (m_spDiagnostics)
		m_spDiagnostics->phase("process_output");
	KFrameMetadata metadata;
	metadata.nRtpTimestamp = frame.rtp_timestamp();
	metadata.nNtpTimeMs = frame.ntp_time_ms();
	metadata.nCaptureTimeMs = frame.timestamp_us() / 1000;
	metadata.nEncodeStartMs = nEncodeStartMs;
	metadata.colorSpace = frame.color_space();
	m_pendingFrames.emplace(m_encoder.lastSubmittedPts(), std::move(metadata));

	if (packets.empty())
	{
		bDiagnosticSuccess = true;
		if (m_spDiagnostics)
			m_spDiagnostics->noOutput();
		++m_nNoOutputCount;
		if (m_nNoOutputCount == 1 || m_nNoOutputCount % kVideoTraceFrameInterval == 0)
		{
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("h264_encode_no_output"),
				QStringLiteral("encoder=%1 noOutputCount=%2 keyframe=%3")
					.arg(m_encoder.encoderName())
					.arg(m_nNoOutputCount)
					.arg(bForceKeyFrame ? 1 : 0));
		}
		return WEBRTC_VIDEO_CODEC_OK;
	}

	bDiagnosticSuccess = emitPackets(packets, nEncodeFinishMs, encodeTimer.elapsed());
	return bDiagnosticSuccess ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_ERROR;
}

bool KWebRtcH264Encoder::emitPackets(const std::map<std::int64_t, QByteArray> &packets,
	qint64 nEncodeFinishMs, qint64 nEncodeCostMs)
{
	for (const auto &[nPts, encodedFrame] : packets)
	{
		const auto pending = m_pendingFrames.find(nPts);
		if (pending == m_pendingFrames.end())
		{
			if (m_spDiagnostics)
				m_spDiagnostics->error(QStringLiteral("reason=unknown_output_pts pts=%1").arg(nPts));
			m_pendingFrames.clear();
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("h264_encode_failed"), QStringLiteral("error=unknown_output_pts pts=%1").arg(nPts));
			return false;
		}
		QByteArray annexBFrame = normalizeToAnnexB(encodedFrame);
		if (annexBFrame.isEmpty())
		{
			if (m_spDiagnostics)
				m_spDiagnostics->error(QStringLiteral("reason=normalize_annexb_failed"));
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("h264_encode_failed"),
				QStringLiteral("encoder=%1 error=normalize_annexb_failed bytes=%2")
					.arg(m_encoder.encoderName())
					.arg(encodedFrame.size()));
			m_pendingFrames.clear();
			return false;
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
		QString strLevelError;
		const int nPreviousLevel = m_spsGuard.observedLevel();
		bool bValidSps = m_spsGuard.validate(std::span(reinterpret_cast<const uint8_t *>(annexBFrame.constData()),
			static_cast<size_t>(annexBFrame.size())), m_nWidth, m_nHeight, m_nConfiguredFps, &strLevelError);
		if (bValidSps && m_spsGuard.observedLevel() < m_nEncodingLevel)
		{
			strLevelError = QStringLiteral("H.264 SPS level=%1 cannot cover configured rate/buffer envelope level=%2")
				.arg(m_spsGuard.observedLevel()).arg(m_nEncodingLevel);
			bValidSps = false;
		}
		if (!bValidSps)
		{
			if (m_spDiagnostics)
				m_spDiagnostics->error(QStringLiteral("reason=sps_rejected detail=%1").arg(strLevelError));
			m_bLevelError = true;
			m_pendingFrames.clear();
			qWarning() << strLevelError;
			KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_level_rejected"), strLevelError);
			return false;
		}
		if (m_spsGuard.observedLevel() != nPreviousLevel)
			KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_sps_validated"),
				QStringLiteral("actualLevel=%1 negotiatedLevel=%2 encodingLevel=%3")
					.arg(m_spsGuard.observedLevel()).arg(m_nNegotiatedLevel).arg(m_nEncodingLevel));

		++m_nEncodedFrameCount;
		if (m_nEncodedFrameCount % kVideoTraceFrameInterval == 0)
		{
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("h264_encode_end"),
				QStringLiteral("encoder=%1 frame=%2 costMs=%3 bytes=%4 keyframe=%5 requestedBitrateKbps=%6")
					.arg(m_encoder.encoderName())
					.arg(m_nEncodedFrameCount)
					.arg(nEncodeCostMs)
					.arg(annexBFrame.size())
					.arg(bKeyFrame ? 1 : 0)
					.arg(m_nBitrateKbps));
		}

		const bool bEmitted = emitEncodedFrame(pending->second, annexBFrame,
			pending->second.nEncodeStartMs, nEncodeFinishMs);
		m_pendingFrames.erase(pending);
		if (!bEmitted)
		{
			m_pendingFrames.clear();
			return false;
		}
	}
	return true;
}

void KWebRtcH264Encoder::SetRates(const webrtc::VideoEncoder::RateControlParameters &parameters)
{
	const int nBitrateKbps = static_cast<int>(parameters.bitrate.get_sum_bps() / kBitsPerKilobit);
	const QString strRateDetails = m_spDiagnostics
		? QStringLiteral("encoder=%1 requestedBps=%2 requestedFps=%3 configuredFps=%4 encodingLevel=%5 levelError=%6 width=%7 height=%8")
			.arg(m_encoder.encoderName()).arg(parameters.bitrate.get_sum_bps()).arg(parameters.framerate_fps, 0, 'g', 12)
			.arg(m_nConfiguredFps).arg(m_nEncodingLevel).arg(m_bLevelError).arg(m_nWidth).arg(m_nHeight)
		: QString();
	if (m_spDiagnostics)
		m_spDiagnostics->rates(strRateDetails);
	if (nBitrateKbps > 0)
	{
		if (!KH264LevelPolicy::fits(m_nEncodingLevel, m_nWidth, m_nHeight, m_nConfiguredFps,
			static_cast<std::int64_t>(nBitrateKbps) * 1000, static_cast<std::int64_t>(nBitrateKbps) * 500, 1))
		{
			m_bLevelError = true;
			qWarning() << "H.264 rate update exceeds configured level; renegotiation required";
			if (m_spDiagnostics)
				m_spDiagnostics->error(QStringLiteral("reason=rate_exceeds_level %1").arg(strRateDetails));
			return;
		}
		m_nBitrateKbps = nBitrateKbps;
		m_encoder.setBitrateKbps(m_nBitrateKbps);
	}

	// WebRTC derives this feedback from the input cadence; transient overshoot
	// is not a stream configuration change or evidence of an invalid SPS.
	// Bound before converting to int, and use InitEncode's FPS for unavailable feedback.
	int nRequestedFps = m_nConfiguredFps;
	if (std::isfinite(parameters.framerate_fps) && parameters.framerate_fps > 0.0)
	{
		const double fBoundedFps = std::min(parameters.framerate_fps,
			static_cast<double>(m_nConfiguredFps));
		nRequestedFps = clampFps(static_cast<int>(std::round(fBoundedFps)));
	}
	if (nRequestedFps == m_nTargetFps)
		return;

	m_nTargetFps = nRequestedFps;
	++m_nRateTargetChangeCount;
	if (m_nRateTargetChangeCount == 1
		|| m_nRateTargetChangeCount % kRateTargetTraceInterval == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("h264_rate_target_changed"),
			QStringLiteral("configuredFps=%1 targetFps=%2 requestedBitrateKbps=%3 changeCount=%4")
				.arg(m_nConfiguredFps)
				.arg(m_nTargetFps)
				.arg(m_nBitrateKbps)
				.arg(m_nRateTargetChangeCount));
	}
}

webrtc::VideoEncoder::EncoderInfo KWebRtcH264Encoder::GetEncoderInfo() const
{
	webrtc::VideoEncoder::EncoderInfo info;
	const QString strEncoderName = m_encoder.encoderName();
	info.implementation_name = strEncoderName.isEmpty()
		? "ffmpeg_h264"
		: strEncoderName.toStdString();
	info.is_hardware_accelerated = strEncoderName == QStringLiteral("h264_mf")
		|| strEncoderName == QStringLiteral("h264_qsv") || strEncoderName == QStringLiteral("h264_amf")
		|| strEncoderName == QStringLiteral("h264_nvenc");
	info.supports_native_handle = m_spFramePath && m_spFramePath->mode() != CpuVideoFramePath;
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

bool KWebRtcH264Encoder::emitEncodedFrame(const KFrameMetadata &metadata,
	const QByteArray &encodedData,
	qint64 nEncodeStartMs,
	qint64 nEncodeFinishMs)
{
	if (m_pCallback == nullptr || encodedData.isEmpty())
		return false;

	webrtc::EncodedImage image;
	image._encodedWidth = static_cast<uint32_t>(m_nWidth);
	image._encodedHeight = static_cast<uint32_t>(m_nHeight);
	image.SetRtpTimestamp(metadata.nRtpTimestamp);
	image.ntp_time_ms_ = metadata.nNtpTimeMs;
	image.capture_time_ms_ = metadata.nCaptureTimeMs;
	image.SetColorSpace(metadata.colorSpace);
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

	if (m_spDiagnostics)
		m_spDiagnostics->phase("output_callback");
	const webrtc::EncodedImageCallback::Result result =
		m_pCallback->OnEncodedImage(image, &codecInfo);
	if (m_spDiagnostics)
	{
		m_spDiagnostics->output(result.error == webrtc::EncodedImageCallback::Result::OK,
			static_cast<quint64>(encodedData.size()), bKeyFrame);
		m_spDiagnostics->phase("process_output");
		if (result.error != webrtc::EncodedImageCallback::Result::OK)
			m_spDiagnostics->error(QStringLiteral("reason=callback_failed code=%1").arg(static_cast<int>(result.error)));
	}
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

KWebRtcH264EncoderFactory::KWebRtcH264EncoderFactory(KVideoEncoderPreference preference,
	std::shared_ptr<KVideoFramePathState> spFramePath)
	: m_preference(preference)
	, m_spFramePath(std::move(spFramePath))
{
}

std::vector<webrtc::SdpVideoFormat> KWebRtcH264EncoderFactory::GetSupportedFormats() const
{
	return { SupportedH264Format() };
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

	return std::make_unique<KWebRtcH264Encoder>(m_preference, NegotiatedH264Level(format), m_spFramePath);
}

bool KWebRtcH264EncoderFactory::isH264Format(const webrtc::SdpVideoFormat &format)
{
	return NegotiatedH264Level(format) != 0;
}
