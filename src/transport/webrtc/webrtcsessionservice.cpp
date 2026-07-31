#include "transport/webrtc/webrtcsessionservice.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "input/inputinjector.h"
#include "transport/webrtc/webrtcsignaling.h"

#include <QtCore/QBuffer>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <Windows.h>

namespace
{
	constexpr int kWallpaperInitialMaxEdge = 640;
	constexpr int kWallpaperMinEdge = 240;
	constexpr int kWallpaperJpegQuality = 65;
	constexpr int kWallpaperMaxBase64Bytes = 96 * 1024;
	constexpr int kWallpaperPathBufferLength = MAX_PATH;
	constexpr int kDisconnectGraceMs = 5000;

	static QString roleToString(const QString &strRole)
	{
		return strRole == QStringLiteral("controller")
			? QStringLiteral("controller")
			: QStringLiteral("controlled");
	}

	static bool shouldTraceInputMessage(const KInputMessage &message)
	{
		return message.bTrace
			|| (message.type == KeyInputMessageType && KLatencyTraceLogger::isEnabled());
	}

	static QString inputTraceExtra(const KInputMessage &message)
	{
		QString strExtra = QStringLiteral("seq=%1 type=%2")
			.arg(message.nSequence)
			.arg(KInputMessageCodec::typeName(message.type));
		if (message.type == KeyInputMessageType)
			strExtra += QStringLiteral(" pressed=%1").arg(message.bPressed ? 1 : 0);
		return strExtra;
	}
}

KWebRtcSessionService::KWebRtcSessionService(QObject *pParent)
	: QObject(pParent)
	, m_pSignaling(new KWebRtcSignaling(this))
	, m_pInputInjector(new KInputInjector(this))
	, m_pDisconnectGraceTimer(new QTimer(this))
{
	m_pDisconnectGraceTimer->setSingleShot(true);
	connect(m_pDisconnectGraceTimer, &QTimer::timeout,
		this, &KWebRtcSessionService::handleDisconnectGraceTimeout);
	connect(m_pSignaling, &KWebRtcSignaling::stateChanged,
		this, &KWebRtcSessionService::signalingChanged);
	connect(m_pSignaling, &KWebRtcSignaling::signalingError,
		this, &KWebRtcSessionService::sessionError);
	connect(m_pSignaling, &KWebRtcSignaling::outgoingConnectionEstablished,
		this, &KWebRtcSessionService::handleOutgoingConnectionEstablished);
	connect(m_pSignaling, &KWebRtcSignaling::outgoingConnectionFailed,
		this, &KWebRtcSessionService::handleOutgoingConnectionFailed);
	connect(m_pSignaling, &KWebRtcSignaling::incomingConnectionEstablished,
		this, &KWebRtcSessionService::handleIncomingConnectionEstablished);
	connect(m_pSignaling, &KWebRtcSignaling::connectionLost,
		this, &KWebRtcSessionService::handleSignalingConnectionLost);
	connect(m_pInputInjector, &KInputInjector::inputError,
		this, &KWebRtcSessionService::sessionError);
}

KWebRtcSessionService::~KWebRtcSessionService()
{
	disconnectSession();
}

void KWebRtcSessionService::setRole(const QString &strRole)
{
	if (strRole != QStringLiteral("controlled") && strRole != QStringLiteral("controller"))
		return;
	if (strRole != m_strRole && m_sessionState != IdleSessionState)
		finishSession(QStringLiteral("role_changed"), false, true, false);
	m_strRole = strRole;

	emit webRtcStateChanged(QStringLiteral("Role:%1").arg(m_strRole));
}

void KWebRtcSessionService::startSignalingServer(quint16 nPort)
{
	if (m_sessionState != IdleSessionState)
		finishSession(QStringLiteral("restart_listener"), false, true, false);
	m_bControllerConnectionPending = false;
	QString strError;
	if (!initializePeer(KWebRtcPeer::ControlledRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	if (!m_pSignaling->startServer(nPort, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_strRole = QStringLiteral("controlled");
	m_sessionState = ListeningSessionState;
	emit webRtcStateChanged(QStringLiteral("Listening"));
}

void KWebRtcSessionService::connectSignaling(const QString &strHost, quint16 nPort)
{
	if (m_sessionState != IdleSessionState)
		finishSession(QStringLiteral("new_connection"), false, true, false);
	m_bControllerConnectionPending = false;
	m_pSignaling->stop();
	QString strError;
	if (!initializePeer(KWebRtcPeer::ControllerRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_bControllerConnectionPending = true;
	m_strRole = QStringLiteral("controller");
	m_sessionState = ConnectingSessionState;
	emit webRtcStateChanged(QStringLiteral("Connecting"));
	m_pSignaling->connectToHost(strHost, nPort);
}

void KWebRtcSessionService::disconnectSession()
{
	finishSession(QStringLiteral("local_disconnect"), false, true, false);
}

void KWebRtcSessionService::enterRemoteDesktop(const KStreamConfig &config)
{
	if (m_strRole != QStringLiteral("controller")
		|| !m_bSessionChannelOpen
		|| m_sessionState != ConnectedSessionState)
	{
		return;
	}

	KSessionTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("startup_order"),
		QStringLiteral("streamConfigBeforeStart"),
		-1,
		QStringLiteral("width=%1 height=%2 fps=%3 bitrateKbps=%4")
			.arg(config.nWidth)
			.arg(config.nHeight)
			.arg(config.nFps)
			.arg(config.nBitrateKbps));
	sendStreamConfig(config);
	KSessionMessage message;
	message.type = StartStreamingSessionMessageType;
	sendSessionMessage(message);
	m_bStreaming = true;
	m_sessionState = StreamingSessionState;
	emit webRtcStateChanged(QStringLiteral("Streaming"));
}

void KWebRtcSessionService::leaveRemoteDesktop()
{
	if (m_strRole != QStringLiteral("controller")
		|| m_sessionState != StreamingSessionState)
	{
		return;
	}
	KSessionMessage message;
	message.type = StopStreamingSessionMessageType;
	sendSessionMessage(message);
	m_bStreaming = false;
	m_sessionState = ConnectedSessionState;
	emit stopCaptureRequested();
	emit webRtcStateChanged(QStringLiteral("Connected"));
}

void KWebRtcSessionService::startStreaming()
{
	if (m_strRole != QStringLiteral("controlled"))
	{
		emit sessionError(QStringLiteral("只有被控端可以开始推流"));
		return;
	}
	if (!m_bSessionChannelOpen || m_sessionState != ConnectedSessionState)
		return;

	emit startCaptureRequested();
	m_bInputAllowed = true;
	m_bStreaming = true;
	m_sessionState = StreamingSessionState;
	emit webRtcStateChanged(QStringLiteral("Streaming"));
}

void KWebRtcSessionService::stopStreaming()
{
	if (m_strRole == QStringLiteral("controlled"))
	{
		finishSession(QStringLiteral("controlled_user_stop"), true, true, false);
		return;
	}

	leaveRemoteDesktop();
}

void KWebRtcSessionService::pushVideoFrame(const KWebRtcVideoFrame &frame)
{
	if (m_pPeer != nullptr && m_bStreaming && m_sessionState == StreamingSessionState)
		m_pPeer->pushVideoFrame(frame);
}

void KWebRtcSessionService::sendInputMessage(const KInputMessage &message)
{
	if (m_strRole != QStringLiteral("controller")
		|| !m_bStreaming
		|| !m_bInputChannelOpen
		|| m_sessionState != StreamingSessionState)
	{
		return;
	}
	if (m_pPeer == nullptr)
		return;

	const QString strMessage = KInputMessageCodec::encode(message);
	if (shouldTraceInputMessage(message))
	{
		KLatencyTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("input_send"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}
	m_pPeer->sendInputMessage(strMessage);
}

void KWebRtcSessionService::sendSessionMessage(const KSessionMessage &message)
{
	const QString strType = KSessionMessageCodec::typeName(message.type);
	const QString strMessage = KSessionMessageCodec::encode(message);
	if (m_pPeer == nullptr || !m_bSessionChannelOpen)
	{
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("send_drop"),
			strType,
			strMessage.toUtf8().size(),
			QStringLiteral("reason=session_channel_not_open"));
		return;
	}

	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("send"),
		strType,
		strMessage.toUtf8().size());
	m_pPeer->sendSessionMessage(strMessage);
}

void KWebRtcSessionService::sendStreamConfig(const KStreamConfig &config)
{
	if (m_strRole != QStringLiteral("controller"))
		return;
	KSessionMessage message;
	message.type = StreamConfigSessionMessageType;
	message.streamConfig = config;
	sendSessionMessage(message);
}

void KWebRtcSessionService::handleCaptureFailure()
{
	if (m_strRole != QStringLiteral("controlled")
		|| m_sessionState != StreamingSessionState
		|| m_bEndingSession)
	{
		return;
	}

	finishSession(QStringLiteral("capture_failed"), true, true, true);
}

void KWebRtcSessionService::finishSession(const QString &strReason,
	bool bKeepListening,
	bool bNotifyRemote,
	bool bReportError)
{
	if (m_bEndingSession)
		return;

	m_bEndingSession = true;
	m_sessionState = StoppingSessionState;
	emit webRtcStateChanged(QStringLiteral("Stopping"));
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("session_end"),
		strReason,
		-1,
		QStringLiteral("keepListening=%1 notifyRemote=%2")
			.arg(bKeepListening ? 1 : 0)
			.arg(bNotifyRemote ? 1 : 0));

	if (bNotifyRemote && m_bSessionChannelOpen)
	{
		KSessionMessage message;
		message.type = EndSessionMessageType;
		message.strReason = strReason;
		sendSessionMessage(message);
	}

	m_pDisconnectGraceTimer->stop();
	m_bControllerConnectionPending = false;
	m_bDeviceInfoRequested = false;
	m_bInputAllowed = false;
	m_bInputChannelOpen = false;
	m_bSessionChannelOpen = false;
	m_bStreaming = false;
	m_pInputInjector->releaseAllInputs();
	resetInputTraceState();
	emit stopCaptureRequested();
	emit sessionChannelChanged(false);
	emit networkStatsReady(KWebRtcNetworkStats());

	if (m_pPeer != nullptr)
		m_pPeer->shutdown();

	bool bListening = false;
	if (bKeepListening && m_strRole == QStringLiteral("controlled"))
	{
		m_pSignaling->disconnectPeer();
		QString strError;
		if (initializePeer(KWebRtcPeer::ControlledRole, &strError))
		{
			bListening = true;
			m_sessionState = ListeningSessionState;
			emit webRtcStateChanged(QStringLiteral("Listening"));
		}
		else
		{
			m_pSignaling->stop();
			m_sessionState = IdleSessionState;
			emit webRtcStateChanged(QStringLiteral("Failed"));
			emit sessionError(strError);
		}
	}
	else
	{
		m_pSignaling->stop();
		m_sessionState = IdleSessionState;
		emit webRtcStateChanged(QStringLiteral("Disconnected"));
	}

	m_bEndingSession = false;
	if (bReportError)
	{
		emit sessionError(QStringLiteral("Remote session ended: %1").arg(strReason));
	}
	else if (bListening)
	{
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("session_ready"),
			QStringLiteral("listening_after_end"));
	}
}

void KWebRtcSessionService::resetInputTraceState()
{
	m_nLastInjectedInputSeq = 0;
	m_nLastInjectedInputMs = -1;
	emit inputTraceUpdated(0, -1);
}

bool KWebRtcSessionService::initializePeer(KWebRtcPeer::Role role, QString *pErrorMessage)
{
	m_bDeviceInfoRequested = false;
	if (m_pPeer == nullptr)
	{
		m_pPeer = new KWebRtcPeer(this);
		wirePeer();
	}

	m_pPeer->shutdown();
	return m_pPeer->initialize(role, pErrorMessage);
}

void KWebRtcSessionService::wirePeer()
{
	connect(m_pPeer, &KWebRtcPeer::signalingMessageReady,
		m_pSignaling, &KWebRtcSignaling::sendJsonMessage);
	connect(m_pSignaling, &KWebRtcSignaling::messageReceived,
		m_pPeer, &KWebRtcPeer::handleSignalingMessage);
	connect(m_pPeer, &KWebRtcPeer::stateChanged,
		this, &KWebRtcSessionService::webRtcStateChanged);
	connect(m_pPeer, &KWebRtcPeer::peerError,
		this, &KWebRtcSessionService::sessionError);
	connect(m_pPeer, &KWebRtcPeer::remoteFrameReady,
		this, &KWebRtcSessionService::handleRemoteFrame, Qt::DirectConnection);
	connect(m_pPeer, &KWebRtcPeer::networkStatsReady,
		this, &KWebRtcSessionService::networkStatsReady);
	connect(m_pPeer, &KWebRtcPeer::inputMessageReceived,
		this, &KWebRtcSessionService::handleInputMessage);
	connect(m_pInputInjector, &KInputInjector::inputInjected,
		this, &KWebRtcSessionService::handleInputInjected);
	connect(m_pPeer, &KWebRtcPeer::inputChannelChanged,
		this, &KWebRtcSessionService::handleInputChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::sessionMessageReceived,
		this, &KWebRtcSessionService::handleSessionMessage);
	connect(m_pPeer, &KWebRtcPeer::sessionChannelChanged,
		this, &KWebRtcSessionService::handleSessionChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::sessionChannelChanged,
		this, &KWebRtcSessionService::sessionChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::peerConnectionInterrupted,
		this, &KWebRtcSessionService::handlePeerConnectionInterrupted);
	connect(m_pPeer, &KWebRtcPeer::peerConnectionRestored,
		this, &KWebRtcSessionService::handlePeerConnectionRestored);
	connect(m_pPeer, &KWebRtcPeer::peerConnectionTerminated,
		this, &KWebRtcSessionService::handlePeerConnectionTerminated);
}

void KWebRtcSessionService::handleRemoteFrame(const KDecodedVideoFrame &frame)
{
	// The peer already coalesces remote frames (drop-old) and the render widget
	// coalesces again before present, so this middle layer only forwards the
	// frame instead of adding another mutex+QueuedConnection hop.
	if (frame.nWidth <= 0 || frame.nHeight <= 0 || frame.vecBgraBuffer.empty())
		return;

	emit remoteFrameReady(frame);
	emit remoteFrameStatsReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
}

void KWebRtcSessionService::handleInputMessage(const QString &strMessage)
{
	KInputMessage message;
	QString strError;
	if (!KInputMessageCodec::decode(strMessage, &message, &strError))
	{
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("protocol_reject"),
			QStringLiteral("input"),
			strMessage.toUtf8().size(),
			strError);
		return;
	}

	if (shouldTraceInputMessage(message))
	{
		KLatencyTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("input_recv"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}

	if (m_strRole != QStringLiteral("controlled")
		|| !m_bInputAllowed
		|| !m_bStreaming
		|| m_sessionState != StreamingSessionState)
	{
		return;
	}

	m_pInputInjector->handleInputMessage(message);
}

void KWebRtcSessionService::handleInputChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bInputChannelOpen;
	m_bInputChannelOpen = bOpen;
	if (!bOpen)
		m_pInputInjector->releaseAllInputs();
	emit inputChannelChanged(bOpen);
	if (bWasOpen && !bOpen && !m_bEndingSession)
	{
		finishSession(QStringLiteral("input_channel_closed"),
			m_strRole == QStringLiteral("controlled"),
			false,
			true);
	}
}

void KWebRtcSessionService::handleSessionChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bSessionChannelOpen;
	m_bSessionChannelOpen = bOpen;
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("channel"),
		QStringLiteral("session"),
		-1,
		QStringLiteral("open=%1").arg(bOpen ? 1 : 0));
	if (!bOpen)
	{
		m_bDeviceInfoRequested = false;
		if (bWasOpen && !m_bEndingSession)
		{
			finishSession(QStringLiteral("session_channel_closed"),
				m_strRole == QStringLiteral("controlled"),
				false,
				true);
		}
		return;
	}
	m_sessionState = ConnectedSessionState;
	emit webRtcStateChanged(QStringLiteral("Connected"));
	if (m_strRole == QStringLiteral("controller"))
	{
		if (m_bDeviceInfoRequested)
		{
			KSessionTraceLogger::write(roleToString(m_strRole),
				QStringLiteral("skip"),
				KSessionMessageCodec::typeName(DeviceInfoRequestSessionMessageType),
				-1,
				QStringLiteral("reason=already_requested"));
			return;
		}

		m_bDeviceInfoRequested = true;
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("handle_channel_open"),
			KSessionMessageCodec::typeName(DeviceInfoRequestSessionMessageType));
		KSessionMessage message;
		message.type = DeviceInfoRequestSessionMessageType;
		sendSessionMessage(message);
	}
}

void KWebRtcSessionService::handleSessionMessage(const QString &strMessage)
{
	KSessionMessage message;
	QString strError;
	if (!KSessionMessageCodec::decode(strMessage, &message, &strError))
	{
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("protocol_reject"),
			QStringLiteral("session"),
			strMessage.toUtf8().size(),
			strError);
		return;
	}

	const QString strType = KSessionMessageCodec::typeName(message.type);
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("handle"),
		strType,
		strMessage.toUtf8().size());
	if (message.type == DeviceInfoRequestSessionMessageType
		&& m_strRole == QStringLiteral("controlled"))
	{
		sendDeviceInfoMessage();
		return;
	}

	if (message.type == DeviceInfoSessionMessageType
		&& m_strRole == QStringLiteral("controller"))
	{
		emit remoteDeviceInfoChanged(message.deviceInfo.strComputerName,
			message.deviceInfo.strWallpaperMime,
			message.deviceInfo.strWallpaperData,
			message.deviceInfo.nScreenWidth,
			message.deviceInfo.nScreenHeight);
		return;
	}

	if (message.type == StartStreamingSessionMessageType
		&& m_strRole == QStringLiteral("controlled"))
	{
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("emit"),
			QStringLiteral("startCaptureRequested"));
		emit startCaptureRequested();
		m_bInputAllowed = true;
		m_bStreaming = true;
		m_sessionState = StreamingSessionState;
		emit webRtcStateChanged(QStringLiteral("Streaming"));
		return;
	}

	if (message.type == StopStreamingSessionMessageType
		&& m_strRole == QStringLiteral("controlled"))
	{
		m_bInputAllowed = false;
		m_bStreaming = false;
		m_pInputInjector->releaseAllInputs();
		resetInputTraceState();
		emit stopCaptureRequested();
		m_sessionState = ConnectedSessionState;
		emit webRtcStateChanged(QStringLiteral("Connected"));
		return;
	}

	if (message.type == EndSessionMessageType)
	{
		const QString strRemoteReason = message.strReason;
		finishSession(strRemoteReason.isEmpty()
				? QStringLiteral("remote_stop")
				: QStringLiteral("remote_%1").arg(strRemoteReason),
			m_strRole == QStringLiteral("controlled"),
			false,
			m_strRole == QStringLiteral("controller"));
		return;
	}

	if (message.type == StreamConfigSessionMessageType
		&& m_strRole == QStringLiteral("controlled"))
	{
		if (m_pPeer != nullptr)
			m_pPeer->setStreamConfig(message.streamConfig);
		emit streamConfigChanged(message.streamConfig);
	}
}

void KWebRtcSessionService::handleInputInjected(quint64 nSeq, qint64 nInjectedMs)
{
	m_nLastInjectedInputSeq = nSeq;
	m_nLastInjectedInputMs = nInjectedMs;
	emit inputTraceUpdated(nSeq, nInjectedMs);
	emit inputFeedbackFrameRequested();
}

void KWebRtcSessionService::handleOutgoingConnectionEstablished()
{
	if (!m_bControllerConnectionPending || m_pPeer == nullptr)
		return;

	m_bControllerConnectionPending = false;
	m_strRole = QStringLiteral("controller");
	m_sessionState = NegotiatingSessionState;
	emit webRtcStateChanged(QStringLiteral("Negotiating"));
	m_pPeer->createOffer();
}

void KWebRtcSessionService::handleOutgoingConnectionFailed(const QString &strMessage)
{
	if (!m_bControllerConnectionPending)
		return;

	m_bControllerConnectionPending = false;
	finishSession(QStringLiteral("connect_failed"), false, false, false);
	emit sessionError(strMessage);
}

void KWebRtcSessionService::handleIncomingConnectionEstablished()
{
	if (m_strRole != QStringLiteral("controlled") || m_bEndingSession)
		return;

	m_sessionState = NegotiatingSessionState;
	emit webRtcStateChanged(QStringLiteral("Negotiating"));
}

void KWebRtcSessionService::handleSignalingConnectionLost()
{
	if (m_bEndingSession)
		return;

	if (m_sessionState == ConnectingSessionState
		|| m_sessionState == NegotiatingSessionState)
	{
		finishSession(QStringLiteral("signaling_lost_during_negotiation"),
			m_strRole == QStringLiteral("controlled"),
			false,
			true);
	}
}

void KWebRtcSessionService::handlePeerConnectionInterrupted()
{
	if (m_bEndingSession
		|| m_sessionState == IdleSessionState
		|| m_sessionState == ListeningSessionState
		|| m_sessionState == StoppingSessionState)
	{
		return;
	}

	m_bInputAllowed = false;
	m_pInputInjector->releaseAllInputs();
	m_sessionState = InterruptedSessionState;
	emit webRtcStateChanged(QStringLiteral("Interrupted"));
	m_pDisconnectGraceTimer->start(kDisconnectGraceMs);
}

void KWebRtcSessionService::handlePeerConnectionRestored()
{
	if (m_bEndingSession || m_sessionState != InterruptedSessionState)
		return;

	m_pDisconnectGraceTimer->stop();
	m_bInputAllowed = m_strRole == QStringLiteral("controlled") && m_bStreaming;
	m_sessionState = m_bStreaming ? StreamingSessionState : ConnectedSessionState;
	emit webRtcStateChanged(m_bStreaming
		? QStringLiteral("Streaming")
		: QStringLiteral("Connected"));
}

void KWebRtcSessionService::handlePeerConnectionTerminated(const QString &strReason)
{
	if (m_bEndingSession
		|| m_sessionState == IdleSessionState
		|| m_sessionState == ListeningSessionState)
	{
		return;
	}

	finishSession(strReason,
		m_strRole == QStringLiteral("controlled"),
		false,
		true);
}

void KWebRtcSessionService::handleDisconnectGraceTimeout()
{
	if (m_bEndingSession || m_sessionState != InterruptedSessionState)
		return;

	finishSession(QStringLiteral("ice_disconnected_timeout"),
		m_strRole == QStringLiteral("controlled"),
		false,
		true);
}

void KWebRtcSessionService::sendDeviceInfoMessage()
{
	QString strMimeType;
	const QString strWallpaperData = readWallpaperBase64(&strMimeType);
	KSessionMessage message;
	message.type = DeviceInfoSessionMessageType;
	message.deviceInfo.strComputerName = QSysInfo::machineHostName();
	message.deviceInfo.nScreenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	message.deviceInfo.nScreenHeight = ::GetSystemMetrics(SM_CYSCREEN);
	message.deviceInfo.strWallpaperMime = strMimeType;
	message.deviceInfo.strWallpaperData = strWallpaperData;
	const QString strMessage = KSessionMessageCodec::encode(message);
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("prepare"),
		KSessionMessageCodec::typeName(DeviceInfoSessionMessageType),
		strMessage.toUtf8().size(),
		QStringLiteral("wallpaper=%1 wallpaperBytes=%2 messageBytes=%3")
			.arg(strWallpaperData.isEmpty() ? 0 : 1)
			.arg(strWallpaperData.size())
			.arg(strMessage.toUtf8().size()));
	sendSessionMessage(message);
}

QString KWebRtcSessionService::readWallpaperBase64(QString *pMimeType)
{
	wchar_t szWallpaperPath[kWallpaperPathBufferLength] = {};
	if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER,
			kWallpaperPathBufferLength,
			szWallpaperPath,
			0))
	{
		return QString();
	}

	const QString strWallpaperPath = QString::fromWCharArray(szWallpaperPath);
	if (strWallpaperPath.isEmpty())
		return QString();

	QImageReader imageReader(strWallpaperPath);
	QImage image = imageReader.read();
	if (image.isNull())
		return QString();

	const int nMaxEdge = qMax(image.width(), image.height());
	if (nMaxEdge > kWallpaperInitialMaxEdge)
	{
		image = image.scaled(kWallpaperInitialMaxEdge,
			kWallpaperInitialMaxEdge,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	QByteArray base64Data;
	for (;;)
	{
		QByteArray imageData;
		QBuffer buffer(&imageData);
		if (!buffer.open(QIODevice::WriteOnly))
			return QString();
		if (!image.save(&buffer, "JPG", kWallpaperJpegQuality))
			return QString();

		base64Data = imageData.toBase64();
		if (base64Data.size() <= kWallpaperMaxBase64Bytes)
			break;

		const int nCurrentMaxEdge = qMax(image.width(), image.height());
		if (nCurrentMaxEdge <= kWallpaperMinEdge)
			return QString();

		const int nNextMaxEdge = qMax(kWallpaperMinEdge, nCurrentMaxEdge * 3 / 4);
		image = image.scaled(nNextMaxEdge,
			nNextMaxEdge,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	if (pMimeType != nullptr)
		*pMimeType = QStringLiteral("image/jpeg");
	return QString::fromLatin1(base64Data);
}
