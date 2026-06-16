#include "capture/captureservice.h"

#include "capture/captureworker.h"

#include <QtCore/QThread>

KCaptureService::KCaptureService(QObject *pParent)
	: QObject(pParent)
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
	startCaptureWithMode(KCaptureWorker::WebRtcSourceWorkMode);
}

void KCaptureService::startCaptureWithMode(KCaptureWorker::WorkMode mode)
{
	if (m_pCaptureThread != nullptr)
		return;

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
	connect(m_pCaptureWorker, &KCaptureWorker::webRtcFrameReady,
		this, &KCaptureService::webRtcFrameReady);
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

	m_pCaptureWorker->stopWork();
	m_pCaptureThread->quit();
	m_pCaptureThread->wait();
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
	m_pCaptureThread = nullptr;
	m_pCaptureWorker = nullptr;
}
