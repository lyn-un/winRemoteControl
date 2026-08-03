#include "capture/captureservice.h"

#include "capture/captureworker.h"
#include "common/latencytracelogger.h"

#include <QtCore/QMetaObject>
#include <QtCore/QThread>

namespace
{
	constexpr quint64 kSourceFrameCoalesceTraceInterval = 30;
}

KCaptureService::KCaptureService(QObject *pParent)
	: KCaptureController(pParent)
{
}

KCaptureService::~KCaptureService()
{
	stopCapture();
}

void KCaptureService::startCapture()
{
	startCaptureWithMode(KCaptureWorker::LocalPreviewWorkMode);
}

void KCaptureService::startWebRtcCapture()
{
	startCaptureWithMode(KCaptureWorker::RemoteVideoWorkMode);
}

void KCaptureService::startCaptureWithMode(KCaptureWorker::WorkMode mode)
{
	if (m_pCaptureThread != nullptr)
		return;

	clearPendingWebRtcFrame();
	m_bAcceptWebRtcFrames = (mode == KCaptureWorker::RemoteVideoWorkMode);
	m_pCaptureThread = new QThread(this);
	m_pCaptureWorker = new KCaptureWorker(mode);
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
	if (m_pCaptureThread == nullptr || m_pCaptureWorker == nullptr)
		return;

	m_bAcceptWebRtcFrames = false;
	clearPendingWebRtcFrame();
	m_pCaptureWorker->stopWork();
	m_pCaptureThread->quit();
	m_pCaptureThread->wait();
	clearPendingWebRtcFrame();
	clearWorker();
	emit statusChanged(QStringLiteral("Stopped"));
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
	m_bAcceptWebRtcFrames = false;
	clearPendingWebRtcFrame();
	m_pCaptureThread = nullptr;
	m_pCaptureWorker = nullptr;
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

	emit webRtcFrameReady(frame);

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
