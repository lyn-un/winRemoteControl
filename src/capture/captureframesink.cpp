#include "capture/captureframesink.h"

#include "codec/h264decoder.h"
#include "codec/h264encoder.h"
#include "common/framewatermark.h"
#include "common/latencytracelogger.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <cmath>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace
{
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr qint64 kInitialFrameRetryDurationMs = 1000;
	constexpr int kInitialFrameRetryMaxCount = 10;
}

KCaptureFrameSink::KCaptureFrameSink(SinkMode mode, QObject *pParent)
	: QObject(pParent)
	, m_mode(mode)
{
}

KCaptureFrameSink::~KCaptureFrameSink()
{
	shutdown();
}

bool KCaptureFrameSink::initialize(QString *pErrorMessage)
{
	if (m_mode == RemoteVideoSinkMode)
		return true;

	m_upEncoder = std::make_unique<KH264Encoder>();
	m_upDecoder = std::make_unique<KH264Decoder>();
	if (!m_upDecoder->open(pErrorMessage))
	{
		m_upEncoder.reset();
		m_upDecoder.reset();
		return false;
	}
	return true;
}

bool KCaptureFrameSink::processFrame(KCaptureFrame frame, QString *pErrorMessage)
{
	const KStreamConfig currentConfig = streamConfig();
	const bool bSuccess = m_mode == RemoteVideoSinkMode
		? processRemoteFrame(std::move(frame), currentConfig, pErrorMessage)
		: processLocalPreviewFrame(frame, pErrorMessage);
	return bSuccess;
}

void KCaptureFrameSink::handleCaptureTimeout()
{
	if (m_mode != RemoteVideoSinkMode
		|| !m_initialFrameRetryTimer.isValid()
		|| m_initialFrameRetryTimer.elapsed() > kInitialFrameRetryDurationMs
		|| m_nInitialFrameRetryCount >= kInitialFrameRetryMaxCount
		|| m_lastVideoFrame.nWidth <= 0
		|| m_lastVideoFrame.nHeight <= 0)
	{
		return;
	}

	++m_nInitialFrameRetryCount;
	m_lastVideoFrame.nTimestampMs = QDateTime::currentMSecsSinceEpoch();
	KLatencyTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("initial_frame_retry"),
		QStringLiteral("attempt=%1 sourceFrame=%2 timestampMs=%3")
			.arg(m_nInitialFrameRetryCount)
			.arg(m_lastVideoFrame.nFrameIndex)
			.arg(m_lastVideoFrame.nTimestampMs));
	emit videoFrameReady(m_lastVideoFrame);
}

void KCaptureFrameSink::shutdown()
{
	if (m_bCodecOpen && m_upEncoder != nullptr)
		m_upEncoder->close(nullptr);
	if (m_upDecoder != nullptr)
		m_upDecoder->close();
	m_upEncoder.reset();
	m_upDecoder.reset();
	m_bCodecOpen = false;
	m_lastVideoFrame = KVideoFrame();
	m_initialFrameRetryTimer.invalidate();
	m_nInitialFrameRetryCount = 0;
}

void KCaptureFrameSink::setStreamConfig(const KStreamConfig &config)
{
	std::lock_guard<std::mutex> guard(m_configMutex);
	m_streamConfig = normalizeStreamConfig(config);
}

void KCaptureFrameSink::setInputTraceState(quint64 nSeq, qint64 nInjectedMs)
{
	std::lock_guard<std::mutex> guard(m_inputTraceMutex);
	m_nLastInputSeq = nSeq;
	m_nLastInputInjectedMs = nInjectedMs;
}

KStreamConfig KCaptureFrameSink::streamConfig() const
{
	std::lock_guard<std::mutex> guard(m_configMutex);
	return m_streamConfig;
}

void KCaptureFrameSink::inputTraceState(quint64 *pSeq, qint64 *pInjectedMs) const
{
	std::lock_guard<std::mutex> guard(m_inputTraceMutex);
	if (pSeq != nullptr)
		*pSeq = m_nLastInputSeq;
	if (pInjectedMs != nullptr)
		*pInjectedMs = m_nLastInputInjectedMs;
}

bool KCaptureFrameSink::processRemoteFrame(KCaptureFrame frame,
	const KStreamConfig &config,
	QString *pErrorMessage)
{
	if (frame.nFrameIndex > 0 && frame.nFrameIndex % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("capture_ready"),
			QStringLiteral("frame=%1 width=%2 height=%3 timestampMs=%4")
				.arg(frame.nFrameIndex)
				.arg(frame.nWidth)
				.arg(frame.nHeight)
				.arg(frame.nTimestampMs));
	}

	quint64 nLastInputSeq = 0;
	qint64 nLastInputInjectedMs = -1;
	inputTraceState(&nLastInputSeq, &nLastInputInjectedMs);
	const qint64 nLastInputAgeMs = nLastInputInjectedMs >= 0
		? frame.nTimestampMs - nLastInputInjectedMs : -1;

	KVideoFrame videoFrame;
	if (!convertBgraToI420(frame, config, nLastInputSeq, nLastInputAgeMs, &videoFrame))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Convert BGRA to I420 failed");
		return false;
	}

	m_lastVideoFrame = videoFrame;
	if (!m_initialFrameRetryTimer.isValid())
		m_initialFrameRetryTimer.start();

	if (videoFrame.nFrameIndex > 0 && videoFrame.nFrameIndex % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("frame_converted"),
			QStringLiteral("frame=%1 width=%2 height=%3 timestampMs=%4")
				.arg(videoFrame.nFrameIndex)
				.arg(videoFrame.nWidth)
				.arg(videoFrame.nHeight)
				.arg(videoFrame.nTimestampMs));
	}

	emit videoFrameReady(videoFrame);
	emit frameReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
	return true;
}

bool KCaptureFrameSink::processLocalPreviewFrame(KCaptureFrame &frame, QString *pErrorMessage)
{
	if (m_upEncoder == nullptr || m_upDecoder == nullptr)
		return false;

	quint64 nCurrentFrameIndex = frame.nFrameIndex;
	qint64 nCurrentTimestampMs = frame.nTimestampMs;
	m_bDecodeOk = true;
	if (!m_bCodecOpen)
	{
		if (!m_upEncoder->openStream(frame.nWidth,
				frame.nHeight,
				10,
				[this, pErrorMessage, &nCurrentFrameIndex, &nCurrentTimestampMs](const QByteArray &videoData)
				{
					std::vector<KDecodedVideoFrame> vecDecodedFrames;
					if (!m_upDecoder->decode(videoData,
							nCurrentFrameIndex,
							nCurrentTimestampMs,
							&vecDecodedFrames,
							pErrorMessage))
					{
						m_bDecodeOk = false;
						return;
					}
					for (const KDecodedVideoFrame &decodedFrame : vecDecodedFrames)
						emit decodedFrameReady(decodedFrame);
				},
				pErrorMessage))
		{
			return false;
		}
		m_bCodecOpen = true;
	}

	if (!m_upEncoder->encodeBgraFrame(frame.vecBgraBuffer.data(),
			frame.nWidth,
			frame.nHeight,
			frame.nTimestampMs,
			pErrorMessage)
		|| !m_bDecodeOk)
	{
		return false;
	}

	emit frameReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
	return true;
}

bool KCaptureFrameSink::convertBgraToI420(KCaptureFrame &captureFrame,
	const KStreamConfig &config,
	quint64 nLastInputSeq,
	qint64 nLastInputAgeMs,
	KVideoFrame *pVideoFrame)
{
	if (pVideoFrame == nullptr || captureFrame.nWidth < 2 || captureFrame.nHeight < 2
		|| captureFrame.vecBgraBuffer.empty())
	{
		return false;
	}

	int nWidth = captureFrame.nWidth & ~1;
	int nHeight = captureFrame.nHeight & ~1;
	if (config.nWidth > 0 && config.nHeight > 0)
	{
		const double fScaleX = static_cast<double>(config.nWidth) / captureFrame.nWidth;
		const double fScaleY = static_cast<double>(config.nHeight) / captureFrame.nHeight;
		const double fScale = std::min({ fScaleX, fScaleY, 1.0 });
		nWidth = std::max(2, static_cast<int>(std::floor(captureFrame.nWidth * fScale)) & ~1);
		nHeight = std::max(2, static_cast<int>(std::floor(captureFrame.nHeight * fScale)) & ~1);
	}

	if (KLatencyTraceLogger::isEnabled() && nLastInputSeq > 0)
	{
		KFrameWatermark watermark;
		watermark.nSourceFrameIndex = captureFrame.nFrameIndex;
		watermark.nLastInputSeq = nLastInputSeq;
		watermark.nInputAgeMs = nLastInputAgeMs;
		KFrameWatermarkCodec::writeBgra(&captureFrame.vecBgraBuffer,
			captureFrame.nWidth,
			captureFrame.nHeight,
			watermark);
	}

	pVideoFrame->nWidth = nWidth;
	pVideoFrame->nHeight = nHeight;
	pVideoFrame->nFrameIndex = captureFrame.nFrameIndex;
	pVideoFrame->nTimestampMs = captureFrame.nTimestampMs;
	pVideoFrame->nLastInputSeq = nLastInputSeq;
	pVideoFrame->nInputAgeMs = nLastInputAgeMs;
	pVideoFrame->nStrideY = nWidth;
	pVideoFrame->nStrideU = nWidth / 2;
	pVideoFrame->nStrideV = nWidth / 2;
	pVideoFrame->yPlane.resize(nWidth * nHeight);
	pVideoFrame->uPlane.resize((nWidth / 2) * (nHeight / 2));
	pVideoFrame->vPlane.resize((nWidth / 2) * (nHeight / 2));

	SwsContext *pSwsContext = sws_getContext(captureFrame.nWidth,
		captureFrame.nHeight,
		AV_PIX_FMT_BGRA,
		nWidth,
		nHeight,
		AV_PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR,
		nullptr,
		nullptr,
		nullptr);
	if (pSwsContext == nullptr)
		return false;

	const uint8_t *pSourceData[] = { captureFrame.vecBgraBuffer.data(), nullptr, nullptr, nullptr };
	const int nSourceStride[] = { captureFrame.nWidth * 4, 0, 0, 0 };
	uint8_t *pDestinationData[] = {
		reinterpret_cast<uint8_t *>(pVideoFrame->yPlane.data()),
		reinterpret_cast<uint8_t *>(pVideoFrame->uPlane.data()),
		reinterpret_cast<uint8_t *>(pVideoFrame->vPlane.data()),
		nullptr
	};
	const int nDestinationStride[] = {
		pVideoFrame->nStrideY,
		pVideoFrame->nStrideU,
		pVideoFrame->nStrideV,
		0
	};
	const int nScaledRows = sws_scale(pSwsContext,
		pSourceData,
		nSourceStride,
		0,
		captureFrame.nHeight,
		pDestinationData,
		nDestinationStride);
	sws_freeContext(pSwsContext);
	return nScaledRows == nHeight;
}

KStreamConfig KCaptureFrameSink::normalizeStreamConfig(const KStreamConfig &config)
{
	KStreamConfig normalizedConfig = config;
	normalizedConfig.nFps = std::clamp(normalizedConfig.nFps, 1, 60);
	normalizedConfig.nWidth = std::max(0, normalizedConfig.nWidth);
	normalizedConfig.nHeight = std::max(0, normalizedConfig.nHeight);
	normalizedConfig.nBitrateKbps = std::max(500, normalizedConfig.nBitrateKbps);
	return normalizedConfig;
}
