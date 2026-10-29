#include "capture/captureservice.h"

#include "capture/captureworker.h"
#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"

#include <QtCore/QDebug>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>

namespace
{
	constexpr quint64 kSourceFrameCoalesceTraceInterval = 30;
	constexpr unsigned long kDestructorShutdownTimeoutMs = 2000;
}

KCaptureService::KCaptureService(QObject *pParent, KHdrConversionMode hdrMode,
	std::shared_ptr<KVideoFramePathState> spFramePath)
	: KCaptureController(pParent)
	, m_hdrMode(hdrMode)
	, m_spFramePath(std::move(spFramePath))
{
}

KCaptureService::~KCaptureService()
{
	stopCapture();
	if (m_pCaptureThread == nullptr || !m_pCaptureThread->isRunning())
		return;

	if (QThread::currentThread() != m_pCaptureThread
		&& m_pCaptureThread->wait(kDestructorShutdownTimeoutMs))
	{
		return;
	}

	qCritical() << "Capture thread did not stop before destruction; isolating it";
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("capture_lifecycle"),
		QStringLiteral("destructor_timeout"), -1,
		QStringLiteral("timeoutMs=%1 generation=%2")
			.arg(kDestructorShutdownTimeoutMs)
			.arg(m_nGeneration));
	QObject::disconnect(m_pCaptureWorker, nullptr, this, nullptr);
	QObject::disconnect(m_pCaptureThread, nullptr, this, nullptr);
	m_pCaptureThread->setParent(nullptr);
	m_pCaptureThread = nullptr;
	m_pCaptureWorker = nullptr;
}

void KCaptureService::startCapture()
{
	m_nGeneration = 0;
	startCaptureWithMode(KCaptureWorker::LocalPreviewWorkMode);
}

void KCaptureService::startWebRtcCapture(quint64 nGeneration)
{
	m_nGeneration = nGeneration;
	if (m_spFramePath)
		m_spFramePath->begin(nGeneration);
	startCaptureWithMode(KCaptureWorker::RemoteVideoWorkMode);
}

void KCaptureService::startCaptureWithMode(KCaptureWorker::WorkMode mode)
{
	if (m_pCaptureThread != nullptr)
	{
		if (!m_bAcceptWebRtcFrames)
		{
			m_bStartPending = true;
			m_pendingMode = mode;
		}
		return;
	}

	clearPendingWebRtcFrame();
	m_bAcceptWebRtcFrames = (mode == KCaptureWorker::RemoteVideoWorkMode);
	clearPendingCursorShape();
	{
		std::lock_guard<std::mutex> guard(m_cursorMutex);
		m_nCursorGeneration = m_nGeneration;
		m_bAcceptCursorShapes = m_bAcceptWebRtcFrames;
	}
	m_pCaptureThread = new QThread(this);
	m_pCaptureWorker = new KCaptureWorker(mode, nullptr, m_hdrMode, m_spFramePath);
	m_pCaptureWorker->setStreamConfig(m_streamConfig);
	m_pCaptureWorker->setInputTraceState(m_nLastInputSeq, m_nLastInputInjectedMs);
	m_pCaptureWorker->moveToThread(m_pCaptureThread);

	connect(m_pCaptureThread, &QThread::started,
		m_pCaptureWorker, &KCaptureWorker::startWork);
	connect(m_pCaptureWorker, &KCaptureWorker::statusChanged,
		this, &KCaptureService::statusChanged);
	connect(m_pCaptureWorker, &KCaptureWorker::captureError,
		this, &KCaptureService::captureError);
	connect(m_pCaptureWorker, &KCaptureWorker::decodedFrameReady,
		this, &KCaptureService::decodedFrameReady);
	connect(m_pCaptureWorker, &KCaptureWorker::videoFrameReady,
		this, &KCaptureService::enqueueWebRtcFrame, Qt::QueuedConnection);
	const quint64 nWorkerGeneration = m_nGeneration;
	connect(m_pCaptureWorker, &KCaptureWorker::cursorShapeReady, this,
		[this, nWorkerGeneration](const KCursorShape &shape)
		{
			enqueueCursorShape(nWorkerGeneration, shape);
		}, Qt::DirectConnection);
	connect(m_pCaptureWorker, &KCaptureWorker::frameReady,
		this, &KCaptureService::frameReady);
	connect(m_pCaptureWorker, &KCaptureWorker::workFinished,
		m_pCaptureThread, &QThread::quit);
	connect(m_pCaptureWorker, &KCaptureWorker::workFinished,
		m_pCaptureWorker, &QObject::deleteLater);
	connect(m_pCaptureThread, &QThread::finished,
		m_pCaptureThread, &QObject::deleteLater);
	connect(m_pCaptureThread, &QThread::finished,
		this, &KCaptureService::clearWorker);

	m_pCaptureThread->start();
}

void KCaptureService::stopCapture()
{
	requestStopCapture(m_nGeneration);
}

void KCaptureService::requestStopCapture(quint64 nGeneration)
{
	if (m_pCaptureThread == nullptr || m_pCaptureWorker == nullptr)
	{
		emit captureShutdownFinished(nGeneration);
		return;
	}

	m_nStoppingGeneration = nGeneration;
	m_bStartPending = false;
	m_bAcceptWebRtcFrames = false;
	clearPendingWebRtcFrame();
	clearPendingCursorShape();
	m_pCaptureWorker->stopWork();
}

void KCaptureService::setStreamConfig(const KStreamConfig &config)
{
	m_streamConfig = config;
	if (m_pCaptureWorker != nullptr)
		m_pCaptureWorker->setStreamConfig(config);
}

void KCaptureService::setInputTraceState(quint64 nSeq, qint64 nInjectedMs)
{
	m_nLastInputSeq = nSeq;
	m_nLastInputInjectedMs = nInjectedMs;
	if (m_pCaptureWorker != nullptr)
		m_pCaptureWorker->setInputTraceState(nSeq, nInjectedMs);
}

void KCaptureService::requestImmediateFrame()
{
	if (m_pCaptureWorker != nullptr)
		m_pCaptureWorker->requestImmediateFrame();
}

void KCaptureService::clearWorker()
{
	const quint64 nFinishedGeneration = m_nStoppingGeneration;
	const bool bRestart = m_bStartPending;
	const KCaptureWorker::WorkMode pendingMode = m_pendingMode;
	m_bAcceptWebRtcFrames = false;
	clearPendingWebRtcFrame();
	clearPendingCursorShape();
	m_pCaptureThread = nullptr;
	m_pCaptureWorker = nullptr;
	m_nStoppingGeneration = 0;
	m_bStartPending = false;
	emit statusChanged(QStringLiteral("Stopped"));
	emit captureShutdownFinished(nFinishedGeneration);
	if (bRestart)
		startCaptureWithMode(pendingMode);
}

void KCaptureService::enqueueCursorShape(quint64 nGeneration, const KCursorShape &shape)
{
	if (!shape.isValid() || shape.nGeneration != nGeneration)
		return;
	bool bNeedQueue = false;
	{
		std::lock_guard<std::mutex> guard(m_cursorMutex);
		if (!m_bAcceptCursorShapes || nGeneration != m_nCursorGeneration)
			return;
		m_pendingCursorShape = shape;
		if (!m_bCursorFlushQueued)
		{
			m_bCursorFlushQueued = true;
			bNeedQueue = true;
		}
	}
	if (bNeedQueue)
		QMetaObject::invokeMethod(this, &KCaptureService::flushLatestCursorShape, Qt::QueuedConnection);
}

void KCaptureService::flushLatestCursorShape()
{
	KCursorShape shape;
	quint64 nGeneration = 0;
	{
		std::lock_guard<std::mutex> guard(m_cursorMutex);
		m_bCursorFlushQueued = false;
		if (!m_bAcceptCursorShapes)
			return;
		shape = std::move(m_pendingCursorShape);
		m_pendingCursorShape = {};
		nGeneration = m_nCursorGeneration;
	}
	if (shape.isValid())
		emit cursorShapeReady(nGeneration, shape);
}

void KCaptureService::clearPendingCursorShape()
{
	std::lock_guard<std::mutex> guard(m_cursorMutex);
	m_pendingCursorShape = {};
	m_bAcceptCursorShapes = false;
	// Keep the queued flag until its callback drains; a restart can reuse it.
}

void KCaptureService::enqueueWebRtcFrame(const KVideoFrame &frame)
{
	if (!m_bAcceptWebRtcFrames || frame.nWidth <= 0 || frame.nHeight <= 0)
		return;

	bool bNeedQueue = false;
	bool bDroppedFrame = false;
	quint64 nDroppedFrames = 0;
	{
		std::lock_guard<std::mutex> guard(m_webRtcFrameMutex);
		if (m_bHasPendingWebRtcFrame)
		{
			++m_nDroppedWebRtcSourceFrames;
			bDroppedFrame = true;
		}

		m_pendingWebRtcFrame = frame;
		m_bHasPendingWebRtcFrame = true;
		nDroppedFrames = m_nDroppedWebRtcSourceFrames;
		if (!m_bWebRtcFrameFlushQueued)
		{
			m_bWebRtcFrameFlushQueued = true;
			bNeedQueue = true;
		}
	}

	if (bDroppedFrame
		&& KLatencyTraceLogger::isEnabled()
		&& (nDroppedFrames == 1
			|| nDroppedFrames % kSourceFrameCoalesceTraceInterval == 0))
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("source_frame_coalesced"),
			QStringLiteral("dropped=%1 latestFrame=%2")
				.arg(nDroppedFrames)
				.arg(frame.nFrameIndex));
	}

	if (bNeedQueue)
	{
		QMetaObject::invokeMethod(this,
			&KCaptureService::flushLatestWebRtcFrame,
			Qt::QueuedConnection);
	}
}

void KCaptureService::flushLatestWebRtcFrame()
{
	KVideoFrame frame;
	{
		std::lock_guard<std::mutex> guard(m_webRtcFrameMutex);
		if (!m_bHasPendingWebRtcFrame || !m_bAcceptWebRtcFrames)
		{
			m_bHasPendingWebRtcFrame = false;
			m_bWebRtcFrameFlushQueued = false;
			return;
		}

		frame = std::move(m_pendingWebRtcFrame);
		m_pendingWebRtcFrame = KVideoFrame();
		m_bHasPendingWebRtcFrame = false;
	}

	emit webRtcFrameReady(m_nGeneration, frame);

	bool bNeedQueue = false;
	{
		std::lock_guard<std::mutex> guard(m_webRtcFrameMutex);
		if (m_bHasPendingWebRtcFrame && m_bAcceptWebRtcFrames)
			bNeedQueue = true;
		else
			m_bWebRtcFrameFlushQueued = false;
	}

	if (bNeedQueue)
	{
		QMetaObject::invokeMethod(this,
			&KCaptureService::flushLatestWebRtcFrame,
			Qt::QueuedConnection);
	}
}

void KCaptureService::clearPendingWebRtcFrame()
{
	std::lock_guard<std::mutex> guard(m_webRtcFrameMutex);
	m_pendingWebRtcFrame = KVideoFrame();
	m_bHasPendingWebRtcFrame = false;
	m_bWebRtcFrameFlushQueued = false;
	m_nDroppedWebRtcSourceFrames = 0;
}
