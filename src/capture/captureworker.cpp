#include "capture/captureworker.h"

#include "capture/captureframe.h"
#include "capture/dxgidesktopduplicator.h"
#include "codec/h264decoder.h"
#include "codec/h264encoder.h"
#include "common/framewatermark.h"
#include "common/latencytracelogger.h"

#include <QtCore/QThread>
#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <vector>

#include <libyuv.h>

namespace
{
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr qint64 kImmediateFrameTraceIntervalMs = 500;
}

KCaptureWorker::KCaptureWorker(WorkMode mode, QObject *pParent)
	: QObject(pParent)
	, m_mode(mode)
{
}

KCaptureWorker::~KCaptureWorker()
{
	stopWork();
}

void KCaptureWorker::startWork()
{
	if (m_bRunning.exchange(true))
		return;

	emit statusChanged(QStringLiteral("Capturing"));

	KDxgiDesktopDuplicator duplicator;
	KH264Encoder h264Encoder;
	KH264Decoder h264Decoder;
	bool bCodecOpen = false;
	bool bDecodeOk = true;
	QString strError;
	QElapsedTimer frameTimer;
	if (!duplicator.initialize(&strError))
	{
		m_bRunning = false;
		emit captureError(strError);
		emit statusChanged(QStringLiteral("Error"));
		emit workFinished();
		return;
	}

	if (m_mode == LocalPreviewWorkMode && !h264Decoder.open(&strError))
	{
		m_bRunning = false;
		emit captureError(strError);
		emit statusChanged(QStringLiteral("Error"));
		duplicator.shutdown();
		emit workFinished();
		return;
	}

	while (m_bRunning)
	{
		frameTimer.restart();
		const KStreamConfig currentConfig = streamConfig();
		KCaptureFrame frame;
		strError.clear();
		const KDxgiDesktopDuplicator::CaptureResult result = duplicator.captureNextFrame(&frame, &strError);
		if (result == KDxgiDesktopDuplicator::TimeoutCaptureResult)
			continue;
		if (result == KDxgiDesktopDuplicator::ErrorCaptureResult)
		{
			m_bRunning = false;
			emit captureError(strError);
			emit statusChanged(QStringLiteral("Error"));
			break;
		}

		if (m_mode == WebRtcSourceWorkMode)
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
			const qint64 nLastInputAgeMs =
				nLastInputInjectedMs >= 0 ? frame.nTimestampMs - nLastInputInjectedMs : -1;

			KWebRtcVideoFrame videoFrame;
			if (!convertBgraToI420(frame, currentConfig, nLastInputSeq, nLastInputAgeMs, &videoFrame))
			{
				m_bRunning = false;
				emit captureError(QStringLiteral("Convert BGRA to I420 failed"));
				emit statusChanged(QStringLiteral("Error"));
				break;
			}

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

			emit webRtcFrameReady(videoFrame);
			emit frameReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
			const int nFrameIntervalMs = std::max(1, 1000 / std::max(1, currentConfig.nFps));
			const qint64 nSleepMs = static_cast<qint64>(nFrameIntervalMs) - frameTimer.elapsed();
			waitForNextFrame(nSleepMs);
			continue;
		}

		quint64 nCurrentFrameIndex = frame.nFrameIndex;
		qint64 nCurrentTimestampMs = frame.nTimestampMs;
		bDecodeOk = true;
		if (!bCodecOpen)
		{
			strError.clear();
			if (h264Encoder.openStream(frame.nWidth,
					frame.nHeight,
					10,
					[this, &h264Decoder, &strError, &nCurrentFrameIndex, &nCurrentTimestampMs, &bDecodeOk](
						const QByteArray &videoData)
					{
						std::vector<KDecodedVideoFrame> vecDecodedFrames;
						if (!h264Decoder.decode(videoData,
								nCurrentFrameIndex,
								nCurrentTimestampMs,
								&vecDecodedFrames,
								&strError))
						{
							bDecodeOk = false;
							return;
						}

						for (const KDecodedVideoFrame &decodedFrame : vecDecodedFrames)
							emit decodedFrameReady(decodedFrame);
					},
					&strError))
			{
				bCodecOpen = true;
			}
			else
			{
				m_bRunning = false;
				emit captureError(strError);
				emit statusChanged(QStringLiteral("Error"));
				break;
			}
		}

		strError.clear();
		if (!h264Encoder.encodeBgraFrame(frame.vecBgraBuffer.data(),
				frame.nWidth,
				frame.nHeight,
				frame.nTimestampMs,
				&strError))
		{
			m_bRunning = false;
			emit captureError(strError.isEmpty() ? QStringLiteral("H.264 encode/decode failed") : strError);
			emit statusChanged(QStringLiteral("Error"));
			break;
		}
		if (!bDecodeOk)
		{
			m_bRunning = false;
			emit captureError(strError.isEmpty() ? QStringLiteral("H.264 decode failed") : strError);
			emit statusChanged(QStringLiteral("Error"));
			break;
		}

		emit frameReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);

		const int nFrameIntervalMs = std::max(1, 1000 / std::max(1, currentConfig.nFps));
		const qint64 nSleepMs = static_cast<qint64>(nFrameIntervalMs) - frameTimer.elapsed();
		if (nSleepMs > 0)
			QThread::msleep(static_cast<unsigned long>(nSleepMs));
	}

	if (bCodecOpen)
		h264Encoder.close(nullptr);
	if (m_mode == LocalPreviewWorkMode)
		h264Decoder.close();

	duplicator.shutdown();
	emit workFinished();
}

void KCaptureWorker::stopWork()
{
	m_bRunning = false;
	m_waitCondition.notify_all();
}

void KCaptureWorker::setStreamConfig(const KStreamConfig &config)
{
	std::lock_guard<std::mutex> guard(m_configMutex);
	m_streamConfig = normalizeStreamConfig(config);
}

void KCaptureWorker::setInputTraceState(quint64 nSeq, qint64 nInjectedMs)
{
	std::lock_guard<std::mutex> guard(m_inputTraceMutex);
	m_nLastInputSeq = nSeq;
	m_nLastInputInjectedMs = nInjectedMs;
}

void KCaptureWorker::requestImmediateFrame()
{
	if (m_mode != WebRtcSourceWorkMode || !m_bRunning)
		return;

	std::lock_guard<std::mutex> guard(m_waitMutex);
	m_bImmediateFrameRequested = true;
	if (shouldTraceImmediateFrameRequest())
	{
		m_bImmediateFrameTracePending = true;
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("immediate_frame_requested"),
			QStringLiteral("mode=webrtc"));
	}
	m_waitCondition.notify_all();
}

KStreamConfig KCaptureWorker::streamConfig() const
{
	std::lock_guard<std::mutex> guard(m_configMutex);
	return m_streamConfig;
}

void KCaptureWorker::inputTraceState(quint64 *pSeq, qint64 *pInjectedMs) const
{
	std::lock_guard<std::mutex> guard(m_inputTraceMutex);
	if (pSeq != nullptr)
		*pSeq = m_nLastInputSeq;
	if (pInjectedMs != nullptr)
		*pInjectedMs = m_nLastInputInjectedMs;
}

bool KCaptureWorker::waitForNextFrame(qint64 nSleepMs)
{
	if (nSleepMs <= 0)
		return false;

	std::unique_lock<std::mutex> lock(m_waitMutex);
	const bool bInterrupted = m_waitCondition.wait_for(lock,
		std::chrono::milliseconds(nSleepMs),
		[this]()
		{
			return !m_bRunning || m_bImmediateFrameRequested;
		});

	const bool bWokeForImmediateFrame = bInterrupted && m_bImmediateFrameRequested && m_bRunning;
	const bool bTracePending = m_bImmediateFrameTracePending;
	m_bImmediateFrameRequested = false;
	m_bImmediateFrameTracePending = false;
	lock.unlock();

	if (bWokeForImmediateFrame && bTracePending)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("immediate_frame_wake"),
			QStringLiteral("mode=webrtc"));
	}

	return bWokeForImmediateFrame;
}

bool KCaptureWorker::shouldTraceImmediateFrameRequest()
{
	if (!KLatencyTraceLogger::isEnabled())
		return false;

	if (!m_immediateFrameTraceTimer.isValid())
	{
		m_immediateFrameTraceTimer.start();
		return true;
	}

	if (m_immediateFrameTraceTimer.elapsed() < kImmediateFrameTraceIntervalMs)
		return false;

	m_immediateFrameTraceTimer.restart();
	return true;
}

KStreamConfig KCaptureWorker::normalizeStreamConfig(const KStreamConfig &config)
{
	KStreamConfig normalizedConfig = config;
	normalizedConfig.nFps = std::clamp(normalizedConfig.nFps, 1, 60);
	normalizedConfig.nWidth = std::max(0, normalizedConfig.nWidth);
	normalizedConfig.nHeight = std::max(0, normalizedConfig.nHeight);
	normalizedConfig.nBitrateKbps = std::max(500, normalizedConfig.nBitrateKbps);
	return normalizedConfig;
}

bool KCaptureWorker::convertBgraToI420(const KCaptureFrame &captureFrame,
	const KStreamConfig &config,
	quint64 nLastInputSeq,
	qint64 nLastInputAgeMs,
	KWebRtcVideoFrame *pVideoFrame)
{
	if (pVideoFrame == nullptr || captureFrame.nWidth < 2 || captureFrame.nHeight < 2
		|| captureFrame.vecBgraBuffer.empty())
	{
		return false;
	}

	int nWidth = captureFrame.nWidth & ~1;
	int nHeight = captureFrame.nHeight & ~1;
	std::vector<unsigned char> scaledBgraBuffer;
	const unsigned char *pSrc = captureFrame.vecBgraBuffer.data();
	int nSrcStride = captureFrame.nWidth * 4;
	if (config.nWidth > 0 && config.nHeight > 0)
	{
		const double fScaleX = static_cast<double>(config.nWidth) / static_cast<double>(captureFrame.nWidth);
		const double fScaleY = static_cast<double>(config.nHeight) / static_cast<double>(captureFrame.nHeight);
		const double fScale = std::min({ fScaleX, fScaleY, 1.0 });
		nWidth = std::max(2, static_cast<int>(std::floor(captureFrame.nWidth * fScale)) & ~1);
		nHeight = std::max(2, static_cast<int>(std::floor(captureFrame.nHeight * fScale)) & ~1);
		if ((nWidth != (captureFrame.nWidth & ~1) || nHeight != (captureFrame.nHeight & ~1))
			&& !resizeBgraFrame(captureFrame, nWidth, nHeight, &scaledBgraBuffer))
		{
			return false;
		}
		if (!scaledBgraBuffer.empty())
		{
			pSrc = scaledBgraBuffer.data();
			nSrcStride = nWidth * 4;
		}
	}

	std::vector<unsigned char> traceBgraBuffer;
	if (KLatencyTraceLogger::isEnabled() && nLastInputSeq > 0)
	{
		traceBgraBuffer.resize(static_cast<size_t>(nWidth) * static_cast<size_t>(nHeight) * 4);
		if (!scaledBgraBuffer.empty())
		{
			std::memcpy(traceBgraBuffer.data(), scaledBgraBuffer.data(), traceBgraBuffer.size());
		}
		else
		{
			for (int y = 0; y < nHeight; ++y)
			{
				const unsigned char *pSrcRow = captureFrame.vecBgraBuffer.data()
					+ static_cast<size_t>(y) * static_cast<size_t>(captureFrame.nWidth) * 4;
				unsigned char *pDstRow = traceBgraBuffer.data()
					+ static_cast<size_t>(y) * static_cast<size_t>(nWidth) * 4;
				std::memcpy(pDstRow, pSrcRow, static_cast<size_t>(nWidth) * 4);
			}
		}

		KFrameWatermark watermark;
		watermark.nSourceFrameIndex = captureFrame.nFrameIndex;
		watermark.nLastInputSeq = nLastInputSeq;
		watermark.nInputAgeMs = nLastInputAgeMs;
		KFrameWatermarkCodec::writeBgra(&traceBgraBuffer, nWidth, nHeight, watermark);
		pSrc = traceBgraBuffer.data();
		nSrcStride = nWidth * 4;

		if (captureFrame.nFrameIndex > 0 && captureFrame.nFrameIndex % kVideoTraceFrameInterval == 0)
		{
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("frame_trace"),
				QStringLiteral("frame=%1 timestampMs=%2 lastInputSeq=%3 inputAgeMs=%4")
					.arg(captureFrame.nFrameIndex)
					.arg(captureFrame.nTimestampMs)
					.arg(nLastInputSeq)
					.arg(nLastInputAgeMs));
		}
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

	unsigned char *pYPlane = reinterpret_cast<unsigned char *>(pVideoFrame->yPlane.data());
	unsigned char *pUPlane = reinterpret_cast<unsigned char *>(pVideoFrame->uPlane.data());
	unsigned char *pVPlane = reinterpret_cast<unsigned char *>(pVideoFrame->vPlane.data());

	const int nConvertResult = libyuv::ARGBToI420(pSrc,
		nSrcStride,
		pYPlane,
		pVideoFrame->nStrideY,
		pUPlane,
		pVideoFrame->nStrideU,
		pVPlane,
		pVideoFrame->nStrideV,
		nWidth,
		nHeight);
	return nConvertResult == 0;
}

bool KCaptureWorker::resizeBgraFrame(const KCaptureFrame &captureFrame,
	int nTargetWidth,
	int nTargetHeight,
	std::vector<unsigned char> *pScaledBgraBuffer)
{
	if (pScaledBgraBuffer == nullptr || nTargetWidth <= 0 || nTargetHeight <= 0)
		return false;

	pScaledBgraBuffer->resize(static_cast<size_t>(nTargetWidth) * static_cast<size_t>(nTargetHeight) * 4);
	const unsigned char *pSrc = captureFrame.vecBgraBuffer.data();
	unsigned char *pDst = pScaledBgraBuffer->data();
	const int nScaleResult = libyuv::ARGBScale(pSrc,
		captureFrame.nWidth * 4,
		captureFrame.nWidth,
		captureFrame.nHeight,
		pDst,
		nTargetWidth * 4,
		nTargetWidth,
		nTargetHeight,
		libyuv::kFilterBox);
	return nScaleResult == 0;
}
