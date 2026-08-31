#include "capture/captureworker.h"

#include "capture/dxgidesktopduplicator.h"
#include "common/latencytracelogger.h"
#include "core/protocol/protocolconstraints.h"

#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <chrono>

namespace
{
	constexpr qint64 kImmediateFrameTraceIntervalMs = 500;
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr qint64 kMicrosecondsPerSecond = 1000000;
	constexpr qint64 kNanosecondsPerMicrosecond = 1000;
}

KCaptureWorker::KCaptureWorker(WorkMode mode, QObject *pParent)
	: KCaptureWorker(std::make_unique<KDxgiDesktopDuplicator>(),
		std::make_unique<KCaptureFrameSink>(mode == RemoteVideoWorkMode
			? KCaptureFrameSink::RemoteVideoSinkMode
			: KCaptureFrameSink::LocalPreviewSinkMode),
		pParent)
{
	m_bRemoteVideo = mode == RemoteVideoWorkMode;
}

KCaptureWorker::KCaptureWorker(std::unique_ptr<IKCaptureSource> upSource,
	std::unique_ptr<IKCaptureFrameSink> upSink,
	QObject *pParent)
	: QObject(pParent)
	, m_upSource(std::move(upSource))
	, m_upSink(std::move(upSink))
{
	KCaptureFrameSink *pQtSink = dynamic_cast<KCaptureFrameSink *>(m_upSink.get());
	if (pQtSink != nullptr)
	{
		connect(pQtSink, &KCaptureFrameSink::decodedFrameReady,
			this, &KCaptureWorker::decodedFrameReady);
		connect(pQtSink, &KCaptureFrameSink::videoFrameReady,
			this, &KCaptureWorker::videoFrameReady);
		connect(pQtSink, &KCaptureFrameSink::frameReady,
			this, &KCaptureWorker::frameReady);
	}
}

KCaptureWorker::~KCaptureWorker()
{
	stopWork();
}

void KCaptureWorker::startWork()
{
	if (m_bRunning.exchange(true))
		return;

	m_cadenceClock.start();
	{
		std::lock_guard<std::mutex> guard(m_waitMutex);
		m_bCadenceResetPending = true;
	}
	emit statusChanged(QStringLiteral("Capturing"));
	QString strError;
	if (m_upSource == nullptr || m_upSink == nullptr
		|| !m_upSource->initialize(&strError)
		|| !m_upSink->initialize(&strError))
	{
		m_bRunning = false;
		emit captureError(strError.isEmpty() ? QStringLiteral("Capture pipeline initialization failed") : strError);
		emit statusChanged(QStringLiteral("Error"));
		if (m_upSource != nullptr)
			m_upSource->shutdown();
		emit workFinished();
		return;
	}

	KCaptureFrame frame;
	while (m_bRunning)
	{
		QElapsedTimer frameTimer;
		frameTimer.start();
		QElapsedTimer captureTimer;
		captureTimer.start();
		strError.clear();
		const IKCaptureSource::CaptureResult result = m_upSource->captureNextFrame(&frame, &strError);
		const qint64 nCaptureUs = captureTimer.nsecsElapsed() / 1000;
		if (result == IKCaptureSource::TimeoutCaptureResult)
		{
			m_upSink->handleCaptureTimeout();
			continue;
		}
		if (result == IKCaptureSource::ErrorCaptureResult)
		{
			m_bRunning = false;
			emit captureError(strError);
			emit statusChanged(QStringLiteral("Error"));
			break;
		}
		QElapsedTimer processTimer;
		processTimer.start();
		if (!m_upSink->processFrame(frame, &strError))
		{
			m_bRunning = false;
			emit captureError(strError.isEmpty() ? QStringLiteral("Capture frame processing failed") : strError);
			emit statusChanged(QStringLiteral("Error"));
			break;
		}
		if (KLatencyTraceLogger::isEnabled()
			&& frame.nFrameIndex > 0
			&& frame.nFrameIndex % kVideoTraceFrameInterval == 0)
		{
			KLatencyTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("capture_pipeline_stage"),
				QStringLiteral("frame=%1 captureUs=%2 processUs=%3 totalUs=%4 "
					"bgraCapacity=%5")
					.arg(frame.nFrameIndex)
					.arg(nCaptureUs)
					.arg(processTimer.nsecsElapsed() / 1000)
					.arg(frameTimer.nsecsElapsed() / 1000)
					.arg(frame.vecBgraBuffer.capacity()));
		}

		qint64 nWaitUs = 0;
		{
			std::lock_guard<std::mutex> guard(m_waitMutex);
			const qint64 nNowUs = m_cadenceClock.nsecsElapsed() / kNanosecondsPerMicrosecond;
			if (m_bCadenceResetPending)
			{
				m_nNextFrameDeadlineUs = nNowUs + m_nFrameIntervalUs;
				m_bCadenceResetPending = false;
			}
			else
			{
				m_nNextFrameDeadlineUs += m_nFrameIntervalUs;
				// A frame that missed its deadline by more than one full period
				// resynchronizes from now instead of bursting to catch up.
				if (nNowUs - m_nNextFrameDeadlineUs >= m_nFrameIntervalUs)
					m_nNextFrameDeadlineUs = nNowUs + m_nFrameIntervalUs;
			}
			nWaitUs = m_nNextFrameDeadlineUs - (m_cadenceClock.nsecsElapsed() / kNanosecondsPerMicrosecond);
		}
		waitForNextFrame(nWaitUs);
	}

	m_upSink->shutdown();
	m_upSource->shutdown();
	emit workFinished();
}

void KCaptureWorker::stopWork()
{
	m_bRunning = false;
	m_waitCondition.notify_all();
}

void KCaptureWorker::setStreamConfig(const KStreamConfig &config)
{
	const int nFps = std::clamp(config.nFps,
		KProtocolConstraints::kMinimumStreamFps,
		KProtocolConstraints::kMaximumStreamFps);
	const qint64 nFrameIntervalUs = kMicrosecondsPerSecond / nFps;
	{
		std::lock_guard<std::mutex> guard(m_waitMutex);
		if (nFrameIntervalUs != m_nFrameIntervalUs || m_bCadenceResetPending)
		{
			// Rebuild the cadence from the current time so the old period is
			// never reused after a frame rate change.
			m_nFrameIntervalUs = nFrameIntervalUs;
			m_bCadenceResetPending = true;
			m_waitCondition.notify_all();
		}
	}
	if (m_upSink != nullptr)
		m_upSink->setStreamConfig(config);
}

void KCaptureWorker::setInputTraceState(quint64 nSeq, qint64 nInjectedMs)
{
	if (m_upSink != nullptr)
		m_upSink->setInputTraceState(nSeq, nInjectedMs);
}

void KCaptureWorker::requestImmediateFrame()
{
	if (!m_bRemoteVideo || !m_bRunning)
		return;

	std::lock_guard<std::mutex> guard(m_waitMutex);
	m_bImmediateFrameRequested = true;
	if (shouldTraceImmediateFrameRequest())
	{
		m_bImmediateFrameTracePending = true;
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("immediate_frame_requested"),
			QStringLiteral("mode=remote_video"));
	}
	m_waitCondition.notify_all();
}

bool KCaptureWorker::waitForNextFrame(qint64 nWaitUs)
{
	if (nWaitUs <= 0)
		return false;

	std::unique_lock<std::mutex> lock(m_waitMutex);
	const bool bInterrupted = m_waitCondition.wait_for(lock,
		std::chrono::microseconds(nWaitUs),
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
			QStringLiteral("mode=remote_video"));
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
