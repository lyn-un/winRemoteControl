#include "session/sessionviewmodel.h"

#include "capture/captureservice.h"
#include "transport/webrtc/webrtcsessionservice.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace
{
	constexpr int kRemoteMouseButtonLeft = 1;
	constexpr int kRemoteMouseButtonRight = 2;

	static void sendJsonMessage(KWebRtcSessionService *pService, const QJsonObject &object)
	{
		if (pService == nullptr)
			return;

		pService->sendInputMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
	}
}

KSessionViewModel::KSessionViewModel(QObject *pParent)
	: QObject(pParent)
	, m_pCaptureService(new KCaptureService(this))
	, m_pWebRtcSessionService(new KWebRtcSessionService(this))
{
	initConnections();
}

KSessionViewModel::~KSessionViewModel()
{
	disconnectSession();
	m_pCaptureService->stopCapture();
}

void KSessionViewModel::startLocalPreview()
{
	m_pCaptureService->startCapture();
}

void KSessionViewModel::stopCapture()
{
	m_pCaptureService->stopCapture();
}

void KSessionViewModel::setRole(const QString &strRole)
{
	m_pWebRtcSessionService->setRole(strRole);
}

void KSessionViewModel::startSignalingServer(quint16 nPort)
{
	m_pWebRtcSessionService->startSignalingServer(nPort);
}

void KSessionViewModel::connectSignaling(const QString &strHost, quint16 nPort)
{
	m_pWebRtcSessionService->connectSignaling(strHost, nPort);
}

void KSessionViewModel::disconnectSession()
{
	m_pWebRtcSessionService->disconnectSession();
	emit clearPreviewRequested();
}

void KSessionViewModel::enterRemoteDesktop()
{
	m_pWebRtcSessionService->enterRemoteDesktop();
}

void KSessionViewModel::leaveRemoteDesktop()
{
	m_pWebRtcSessionService->leaveRemoteDesktop();
	emit clearPreviewRequested();
}

void KSessionViewModel::startStreaming()
{
	m_pWebRtcSessionService->startStreaming();
}

void KSessionViewModel::stopStreaming()
{
	m_pWebRtcSessionService->stopStreaming();
}

void KSessionViewModel::sendRemoteMouseMove(int nX, int nY)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("mouseMove"));
	object.insert(QStringLiteral("x"), nX);
	object.insert(QStringLiteral("y"), nY);
	sendJsonMessage(m_pWebRtcSessionService, object);
}

void KSessionViewModel::sendRemoteMouseButton(int nX, int nY, int nButton, bool bPressed)
{
	QString strButton;
	if (nButton == kRemoteMouseButtonLeft)
		strButton = QStringLiteral("left");
	else if (nButton == kRemoteMouseButtonRight)
		strButton = QStringLiteral("right");
	else
		return;

	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("mouseButton"));
	object.insert(QStringLiteral("button"), strButton);
	object.insert(QStringLiteral("pressed"), bPressed);
	object.insert(QStringLiteral("x"), nX);
	object.insert(QStringLiteral("y"), nY);
	sendJsonMessage(m_pWebRtcSessionService, object);
}

void KSessionViewModel::sendRemoteMouseWheel(int nX, int nY, int nDelta)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("mouseWheel"));
	object.insert(QStringLiteral("delta"), nDelta);
	object.insert(QStringLiteral("x"), nX);
	object.insert(QStringLiteral("y"), nY);
	sendJsonMessage(m_pWebRtcSessionService, object);
}

void KSessionViewModel::sendStreamConfig(const KStreamConfig &config)
{
	m_pWebRtcSessionService->sendStreamConfig(config);
}

void KSessionViewModel::handleCaptureStatusChanged(const QString &strStatus)
{
	emit statusChanged(strStatus);
	if (strStatus == QStringLiteral("Stopped") || strStatus == QStringLiteral("Error"))
		emit clearPreviewRequested();
}

void KSessionViewModel::handleWebRtcStateChanged(const QString &strState)
{
	emit webRtcStateChanged(strState);
	if (strState == QStringLiteral("Disconnected") || strState == QStringLiteral("Stopped"))
		emit clearPreviewRequested();
}

void KSessionViewModel::initConnections()
{
	connect(m_pCaptureService, &KCaptureService::statusChanged,
		this, &KSessionViewModel::handleCaptureStatusChanged);
	connect(m_pCaptureService, &KCaptureService::captureError,
		this, &KSessionViewModel::errorOccurred);
	connect(m_pCaptureService, &KCaptureService::frameReady,
		this, &KSessionViewModel::frameReady);
	connect(m_pCaptureService, &KCaptureService::decodedFrameReady,
		this, &KSessionViewModel::renderFrameReady);
	connect(m_pCaptureService, &KCaptureService::webRtcFrameReady,
		m_pWebRtcSessionService, &KWebRtcSessionService::pushVideoFrame);

	connect(m_pWebRtcSessionService, &KWebRtcSessionService::startCaptureRequested,
		m_pCaptureService, &KCaptureService::startWebRtcCapture);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::stopCaptureRequested,
		m_pCaptureService, &KCaptureService::stopCapture);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::streamConfigChanged,
		m_pCaptureService, &KCaptureService::setStreamConfig);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::signalingChanged,
		this, &KSessionViewModel::signalingChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::webRtcStateChanged,
		this, &KSessionViewModel::handleWebRtcStateChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::sessionChannelChanged,
		this, &KSessionViewModel::sessionChannelChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::remoteDeviceInfoChanged,
		this, &KSessionViewModel::remoteDeviceInfoChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::sessionError,
		this, &KSessionViewModel::errorOccurred);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::remoteFrameReady,
		this, &KSessionViewModel::renderFrameReady);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::remoteFrameStatsReady,
		this, &KSessionViewModel::frameReady);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::networkStatsReady,
		this, &KSessionViewModel::networkStatsReady);
}
