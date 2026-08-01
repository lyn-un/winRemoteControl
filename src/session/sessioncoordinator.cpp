#include "session/sessioncoordinator.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "core/input/inputinjectorinterface.h"
#include "core/protocol/accessmessage.h"
#include "core/session/deviceinfoprovider.h"
#include "core/transport/signalingtransport.h"
#include "input/inputinjector.h"

#include <QtCore/QTimer>
#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <utility>

namespace
{
	constexpr int kDisconnectGraceMs = 5000;
	constexpr int kInitialApprovalResponseTimeoutMs = 5000;
	constexpr int kApprovalResponseGraceMs = 5000;

	static QString roleToString(KSessionRole role)
	{
		return KSessionStateMachine::roleName(role);
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

	static QString AccessRejectionError(const QString &strReason)
	{
		if (strReason == QStringLiteral("user_rejected"))
			return QStringLiteral("对方拒绝了远程控制请求");
		if (strReason == QStringLiteral("timeout"))
			return QStringLiteral("对方未在规定时间内确认连接");
		if (strReason == QStringLiteral("remote_access_disabled"))
			return QStringLiteral("对方已关闭远程控制");
		if (strReason == QStringLiteral("busy"))
			return QStringLiteral("对方正在处理其他连接");
		if (strReason == QStringLiteral("cancelled"))
			return QStringLiteral("远程控制请求已取消");
		return QStringLiteral("对方拒绝了无效的连接请求");
	}
}

KSessionCoordinator::KSessionCoordinator(
	std::unique_ptr<IKDeviceInfoProvider> spDeviceInfoProvider,
	std::unique_ptr<IKInputInjector> spInputInjector,
	std::unique_ptr<KRemotePeerTransport> spRemotePeerTransport,
	std::unique_ptr<KSignalingTransport> spSignalingTransport,
	QObject *pParent)
	: KSessionController(pParent)
	, m_spDeviceInfoProvider(std::move(spDeviceInfoProvider))
	, m_spRemotePeerTransport(std::move(spRemotePeerTransport))
	, m_spSignalingTransport(std::move(spSignalingTransport))
	, m_pSignaling(m_spSignalingTransport.get())
	, m_pInputInjector(new KInputInjector(std::move(spInputInjector), this))
	, m_pDisconnectGraceTimer(new QTimer(this))
	, m_pApprovalTimer(new QTimer(this))
{
	Q_ASSERT(m_spRemotePeerTransport != nullptr);
	Q_ASSERT(m_pSignaling != nullptr);
	m_pDisconnectGraceTimer->setSingleShot(true);
	m_pApprovalTimer->setSingleShot(true);
	connect(m_pDisconnectGraceTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleDisconnectGraceTimeout);
	connect(m_pApprovalTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleApprovalTimeout);
	connect(m_pSignaling, &KSignalingTransport::stateChanged,
		this, &KSessionCoordinator::signalingChanged);
	connect(m_pSignaling, &KSignalingTransport::signalingError,
		this, &KSessionCoordinator::sessionError);
	connect(m_pSignaling, &KSignalingTransport::outgoingConnectionEstablished,
		this, &KSessionCoordinator::handleOutgoingConnectionEstablished);
	connect(m_pSignaling, &KSignalingTransport::outgoingConnectionFailed,
		this, &KSessionCoordinator::handleOutgoingConnectionFailed);
	connect(m_pSignaling, &KSignalingTransport::incomingConnectionEstablished,
		this, &KSessionCoordinator::handleIncomingConnectionEstablished);
	connect(m_pSignaling, &KSignalingTransport::connectionLost,
		this, &KSessionCoordinator::handleSignalingConnectionLost);
	connect(m_pInputInjector, &KInputInjector::inputError,
		this, &KSessionCoordinator::sessionError);
	wirePeer();
}

KSessionCoordinator::~KSessionCoordinator()
{
	disconnectSession();
}

void KSessionCoordinator::setRole(const QString &strRole)
{
	KSessionRole role;
	if (!KSessionStateMachine::roleFromString(strRole, &role))
		return;
	if (role != m_sessionStateMachine.role()
		&& m_sessionStateMachine.state() != IdleSessionState)
	{
		finishSession(RoleChangedSessionEndReason, QString(), false, true, false);
	}
	if (m_sessionStateMachine.state() == IdleSessionState)
		m_sessionStateMachine.setRole(role);

	emit webRtcStateChanged(QStringLiteral("Role:%1")
		.arg(KSessionStateMachine::roleName(m_sessionStateMachine.role())));
}

void KSessionCoordinator::startSignalingServer(quint16 nPort)
{
	if (!m_applicationSettings.bRemoteAccessEnabled)
	{
		emit sessionError(QStringLiteral("远程控制已在设置中关闭"));
		return;
	}
	if (m_sessionStateMachine.state() != IdleSessionState)
		finishSession(RestartListenerSessionEndReason, QString(), false, true, false);
	QString strError;
	if (!initializePeer(ControlledSessionRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	if (!m_pSignaling->startServer(nPort, &strError))
	{
		updateListeningAvailability(false);
		emit sessionError(strError);
		return;
	}

	m_sessionStateMachine.beginListening();
	updateListeningAvailability(true, nPort);
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::connectSignaling(const QString &strHost, quint16 nPort)
{
	const QString strTargetHost = strHost;
	if (m_sessionStateMachine.state() != IdleSessionState)
		finishSession(NewConnectionSessionEndReason, QString(), false, true, false);
	m_pSignaling->stop();
	QString strError;
	if (!initializePeer(ControllerSessionRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_sessionStateMachine.beginConnecting();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_pSignaling->connectToHost(strTargetHost, nPort);
}

void KSessionCoordinator::disconnectSession()
{
	finishSession(LocalDisconnectSessionEndReason, QString(), false, true, false);
}

void KSessionCoordinator::applyApplicationSettings(const KApplicationSettings &settings)
{
	const bool bWasEnabled = m_applicationSettings.bRemoteAccessEnabled;
	m_applicationSettings = SanitizeApplicationSettings(settings);
	if (bWasEnabled && !m_applicationSettings.bRemoteAccessEnabled
		&& m_sessionStateMachine.role() == ControlledSessionRole
		&& m_sessionStateMachine.state() != IdleSessionState)
	{
		const bool bPendingApproval = m_sessionStateMachine.isAwaitingApproval()
			&& !m_strAccessRequestId.isEmpty();
		if (bPendingApproval)
		{
			KAccessMessage rejected;
			rejected.type = RejectedAccessMessageType;
			rejected.strRequestId = m_strAccessRequestId;
			rejected.strReason = QStringLiteral("remote_access_disabled");
			sendAccessMessage(rejected);
		}
		finishSession(LocalDisconnectSessionEndReason,
			QStringLiteral("remote_access_disabled"), false, !bPendingApproval, false);
	}
}

void KSessionCoordinator::respondIncomingAccessRequest(
	const QString &strRequestId,
	bool bAccepted)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| !m_sessionStateMachine.isAwaitingApproval()
		|| strRequestId != m_strAccessRequestId
		|| m_nApprovalGeneration != m_sessionStateMachine.generation())
	{
		return;
	}

	if (bAccepted)
		acceptIncomingAccess();
	else
		rejectIncomingAccess(QStringLiteral("user_rejected"), true);
}

void KSessionCoordinator::enterRemoteDesktop(const KStreamConfig &config)
{
	if (!m_sessionStateMachine.canEnterRemoteDesktop() || !m_bSessionChannelOpen)
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
	m_sessionStateMachine.beginStreaming();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::leaveRemoteDesktop()
{
	if (!m_sessionStateMachine.canLeaveRemoteDesktop())
		return;
	KSessionMessage message;
	message.type = StopStreamingSessionMessageType;
	sendSessionMessage(message);
	m_sessionStateMachine.stopStreaming();
	emit stopCaptureRequested();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::startStreaming()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole)
	{
		emit sessionError(QStringLiteral("只有被控端可以开始推流"));
		return;
	}
	if (!m_bSessionChannelOpen || !m_sessionStateMachine.canStartControlledStreaming())
		return;

	emit startCaptureRequested();
	m_sessionStateMachine.beginStreaming();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::stopStreaming()
{
	if (m_sessionStateMachine.role() == ControlledSessionRole)
	{
		if (!m_sessionStateMachine.hasActiveSession())
			return;
		finishSession(ControlledUserStopSessionEndReason, QString(), true, true, false);
		return;
	}

	leaveRemoteDesktop();
}

void KSessionCoordinator::pushVideoFrame(const KVideoFrame &frame)
{
	if (m_sessionStateMachine.canSendVideo())
		m_spRemotePeerTransport->pushVideoFrame(frame);
}

void KSessionCoordinator::sendInputMessage(const KInputMessage &message)
{
	if (!m_sessionStateMachine.canSendInput() || !m_bInputChannelOpen)
	{
		return;
	}
	const QString strMessage = KInputMessageCodec::encode(message);
	if (shouldTraceInputMessage(message))
	{
		KLatencyTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("input_send"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}
	m_spRemotePeerTransport->sendInputMessage(message);
}

void KSessionCoordinator::sendSessionMessage(const KSessionMessage &message)
{
	const QString strType = KSessionMessageCodec::typeName(message.type);
	const QString strMessage = KSessionMessageCodec::encode(message);
	if (!m_bSessionChannelOpen)
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("send_drop"),
			strType,
			strMessage.toUtf8().size(),
			QStringLiteral("reason=session_channel_not_open"));
		return;
	}

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("send"),
		strType,
		strMessage.toUtf8().size());
	m_spRemotePeerTransport->sendSessionMessage(message);
}

void KSessionCoordinator::sendStreamConfig(const KStreamConfig &config)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	KSessionMessage message;
	message.type = StreamConfigSessionMessageType;
	message.streamConfig = config;
	sendSessionMessage(message);
}

void KSessionCoordinator::handleCaptureFailure()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.state() != StreamingSessionState)
	{
		return;
	}

	finishSession(CaptureFailedSessionEndReason, QString(), true, true, true);
}

void KSessionCoordinator::finishSession(KSessionEndReason reason,
	const QString &strDetail,
	bool bKeepListening,
	bool bNotifyRemote,
	bool bReportError)
{
	if (bNotifyRemote
		&& m_sessionStateMachine.isAwaitingApproval()
		&& !m_strAccessRequestId.isEmpty())
	{
		KAccessMessage message;
		message.type = RejectedAccessMessageType;
		message.strRequestId = m_strAccessRequestId;
		message.strReason = QStringLiteral("cancelled");
		sendAccessMessage(message);
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("access"), QStringLiteral("cancelled"), -1,
			QStringLiteral("requestId=%1").arg(m_strAccessRequestId));
	}
	if (!m_sessionStateMachine.beginStopping())
		return;
	updateListeningAvailability(false);

	const QString strReason = KSessionStateMachine::endReasonName(reason, strDetail);
	const KSessionRole role = m_sessionStateMachine.role();
	const quint64 nGeneration = m_sessionStateMachine.generation();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	KSessionTraceLogger::write(roleToString(role),
		QStringLiteral("session_end"),
		strReason,
		-1,
		QStringLiteral("generation=%1 keepListening=%2 notifyRemote=%3")
			.arg(nGeneration)
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
	m_pApprovalTimer->stop();
	m_nDisconnectGraceGeneration = 0;
	clearApprovalState(strReason);
	m_bDeviceInfoRequested = false;
	m_bInputChannelOpen = false;
	m_bSessionChannelOpen = false;
	m_pInputInjector->releaseAllInputs();
	resetInputTraceState();
	emit stopCaptureRequested();
	emit sessionChannelChanged(false);
	emit networkStatsReady(KNetworkStats());
	m_spRemotePeerTransport->shutdown();

	bool bListening = false;
	if (bKeepListening && role == ControlledSessionRole)
	{
		m_pSignaling->disconnectPeer();
		QString strError;
		if (initializePeer(ControlledSessionRole, &strError))
		{
			bListening = true;
			m_sessionStateMachine.finish(true);
			updateListeningAvailability(true, m_nListeningPort);
			emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		}
		else
		{
			m_pSignaling->stop();
			m_sessionStateMachine.finish(false);
			emit webRtcStateChanged(QStringLiteral("Failed"));
			emit sessionError(strError);
		}
	}
	else
	{
		m_pSignaling->stop();
		m_sessionStateMachine.finish(false);
		emit webRtcStateChanged(QStringLiteral("Disconnected"));
	}

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

void KSessionCoordinator::resetInputTraceState()
{
	m_nLastInjectedInputSeq = 0;
	m_nLastInjectedInputMs = -1;
	emit inputTraceUpdated(0, -1);
}

bool KSessionCoordinator::initializePeer(KSessionRole role, QString *pErrorMessage)
{
	m_bDeviceInfoRequested = false;
	m_spRemotePeerTransport->shutdown();
	return m_spRemotePeerTransport->initialize(role, pErrorMessage);
}

void KSessionCoordinator::wirePeer()
{
	KRemotePeerTransport *pTransport = m_spRemotePeerTransport.get();
	connect(pTransport, &KRemotePeerTransport::signalingMessageReady,
		m_pSignaling, &KSignalingTransport::sendMessage);
	connect(m_pSignaling, &KSignalingTransport::messageReceived,
		this, &KSessionCoordinator::handleSignalingMessage);
	connect(pTransport, &KRemotePeerTransport::stateChanged,
		this, &KSessionCoordinator::webRtcStateChanged);
	connect(pTransport, &KRemotePeerTransport::transportError,
		this, &KSessionCoordinator::sessionError);
	connect(pTransport, &KRemotePeerTransport::remoteFrameReady,
		this, &KSessionCoordinator::handleRemoteFrame, Qt::DirectConnection);
	connect(pTransport, &KRemotePeerTransport::networkStatsReady,
		this, &KSessionCoordinator::networkStatsReady);
	connect(pTransport, &KRemotePeerTransport::inputMessageReceived,
		this, &KSessionCoordinator::handleInputMessage);
	connect(m_pInputInjector, &KInputInjector::inputInjected,
		this, &KSessionCoordinator::handleInputInjected);
	connect(pTransport, &KRemotePeerTransport::inputChannelChanged,
		this, &KSessionCoordinator::handleInputChannelChanged);
	connect(pTransport, &KRemotePeerTransport::sessionMessageReceived,
		this, &KSessionCoordinator::handleSessionMessage);
	connect(pTransport, &KRemotePeerTransport::sessionChannelChanged,
		this, &KSessionCoordinator::handleSessionChannelChanged);
	connect(pTransport, &KRemotePeerTransport::sessionChannelChanged,
		this, &KSessionCoordinator::sessionChannelChanged);
	connect(pTransport, &KRemotePeerTransport::connectionInterrupted,
		this, &KSessionCoordinator::handlePeerConnectionInterrupted);
	connect(pTransport, &KRemotePeerTransport::connectionRestored,
		this, &KSessionCoordinator::handlePeerConnectionRestored);
	connect(pTransport, &KRemotePeerTransport::connectionTerminated,
		this, &KSessionCoordinator::handlePeerConnectionTerminated);
}

void KSessionCoordinator::handleRemoteFrame(const KDecodedVideoFrame &frame)
{
	// The peer already coalesces remote frames (drop-old) and the render widget
	// coalesces again before present, so this middle layer only forwards the
	// frame instead of adding another mutex+QueuedConnection hop.
	if (frame.nWidth <= 0 || frame.nHeight <= 0 || frame.vecBgraBuffer.empty())
		return;

	emit remoteFrameReady(frame);
	emit remoteFrameStatsReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
}

void KSessionCoordinator::handleInputMessage(const KInputMessage &message)
{
	if (shouldTraceInputMessage(message))
	{
		const QString strMessage = KInputMessageCodec::encode(message);
		KLatencyTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("input_recv"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}

	if (!m_sessionStateMachine.canReceiveInput())
		return;

	m_pInputInjector->handleInputMessage(message);
}

void KSessionCoordinator::handleInputChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bInputChannelOpen;
	m_bInputChannelOpen = bOpen;
	if (!bOpen)
		m_pInputInjector->releaseAllInputs();
	emit inputChannelChanged(bOpen);
	if (bWasOpen && !bOpen && !m_sessionStateMachine.isStopping())
	{
		finishSession(InputChannelClosedSessionEndReason,
			QString(),
			m_sessionStateMachine.shouldKeepListening(),
			false,
			true);
	}
}

void KSessionCoordinator::handleSessionChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bSessionChannelOpen;
	m_bSessionChannelOpen = bOpen;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("channel"),
		QStringLiteral("session"),
		-1,
		QStringLiteral("open=%1").arg(bOpen ? 1 : 0));
	if (!bOpen)
	{
		m_bDeviceInfoRequested = false;
		if (bWasOpen && !m_sessionStateMachine.isStopping())
		{
			finishSession(SessionChannelClosedSessionEndReason,
				QString(),
				m_sessionStateMachine.shouldKeepListening(),
				false,
				true);
		}
		return;
	}
	if (!m_sessionStateMachine.markConnected())
		return;
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	if (m_sessionStateMachine.role() == ControllerSessionRole)
	{
		if (m_bDeviceInfoRequested)
		{
			KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
				QStringLiteral("skip"),
				KSessionMessageCodec::typeName(DeviceInfoRequestSessionMessageType),
				-1,
				QStringLiteral("reason=already_requested"));
			return;
		}

		m_bDeviceInfoRequested = true;
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("handle_channel_open"),
			KSessionMessageCodec::typeName(DeviceInfoRequestSessionMessageType));
		KSessionMessage message;
		message.type = DeviceInfoRequestSessionMessageType;
		sendSessionMessage(message);
	}
}

void KSessionCoordinator::handleSessionMessage(const KSessionMessage &message)
{
	const QString strMessage = KSessionMessageCodec::encode(message);
	const QString strType = KSessionMessageCodec::typeName(message.type);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("handle"),
		strType,
		strMessage.toUtf8().size());
	if (message.type == DeviceInfoRequestSessionMessageType
		&& m_sessionStateMachine.role() == ControlledSessionRole)
	{
		sendDeviceInfoMessage();
		return;
	}

	if (message.type == DeviceInfoSessionMessageType
		&& m_sessionStateMachine.role() == ControllerSessionRole)
	{
		emit remoteDeviceInfoChanged(message.deviceInfo.strComputerName,
			message.deviceInfo.strWallpaperMime,
			message.deviceInfo.strWallpaperData,
			message.deviceInfo.nScreenWidth,
			message.deviceInfo.nScreenHeight);
		return;
	}

	if (message.type == StartStreamingSessionMessageType
		&& m_sessionStateMachine.canStartControlledStreaming())
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("emit"),
			QStringLiteral("startCaptureRequested"));
		emit startCaptureRequested();
		m_sessionStateMachine.beginStreaming();
		emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		return;
	}

	if (message.type == StopStreamingSessionMessageType
		&& m_sessionStateMachine.role() == ControlledSessionRole
		&& m_sessionStateMachine.state() == StreamingSessionState)
	{
		m_sessionStateMachine.stopStreaming();
		m_pInputInjector->releaseAllInputs();
		resetInputTraceState();
		emit stopCaptureRequested();
		emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		return;
	}

	if (message.type == EndSessionMessageType)
	{
		const QString strRemoteReason = message.strReason;
		finishSession(RemoteStopSessionEndReason,
			strRemoteReason,
			m_sessionStateMachine.shouldKeepListening(),
			false,
			m_sessionStateMachine.role() == ControllerSessionRole);
		return;
	}

	if (message.type == StreamConfigSessionMessageType
		&& m_sessionStateMachine.role() == ControlledSessionRole)
	{
		m_spRemotePeerTransport->setStreamConfig(message.streamConfig);
		emit streamConfigChanged(message.streamConfig);
	}
}

void KSessionCoordinator::handleInputInjected(quint64 nSeq, qint64 nInjectedMs)
{
	m_nLastInjectedInputSeq = nSeq;
	m_nLastInjectedInputMs = nInjectedMs;
	emit inputTraceUpdated(nSeq, nInjectedMs);
	emit inputFeedbackFrameRequested();
}

void KSessionCoordinator::handleOutgoingConnectionEstablished()
{
	if (!m_sessionStateMachine.isConnecting())
		return;

	if (!m_sessionStateMachine.beginAwaitingApproval())
		return;
	m_strAccessRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_nApprovalGeneration = m_sessionStateMachine.generation();
	KAccessMessage message;
	message.type = RequestAccessMessageType;
	message.strRequestId = m_strAccessRequestId;
	message.strDeviceName = m_spDeviceInfoProvider != nullptr
		? m_spDeviceInfoProvider->deviceName()
		: QStringLiteral("Windows device");
	sendAccessMessage(message);
	m_pApprovalTimer->start(kInitialApprovalResponseTimeoutMs);
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	KSessionTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("access"),
		QStringLiteral("request_sent"),
		-1,
		QStringLiteral("requestId=%1 generation=%2")
			.arg(m_strAccessRequestId)
			.arg(m_nApprovalGeneration));
}

void KSessionCoordinator::handleOutgoingConnectionFailed(const QString &strMessage)
{
	if (!m_sessionStateMachine.isConnecting())
		return;

	finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
	emit sessionError(strMessage);
}

void KSessionCoordinator::handleIncomingConnectionEstablished(
	const QString &strSourceAddress,
	quint16 nSourcePort)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.isStopping())
		return;

	if (!m_sessionStateMachine.beginAwaitingApproval())
		return;
	m_strAccessSourceAddress = strSourceAddress;
	m_nApprovalGeneration = m_sessionStateMachine.generation();
	m_pApprovalTimer->start(kInitialApprovalResponseTimeoutMs);
	updateListeningAvailability(false);
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("access"),
		QStringLiteral("connection_received"),
		-1,
		QStringLiteral("source=%1 sourcePort=%2 generation=%3")
			.arg(strSourceAddress)
			.arg(nSourcePort)
			.arg(m_nApprovalGeneration));
}

void KSessionCoordinator::handleSignalingMessage(const QString &strMessage)
{
	if (KAccessMessageCodec::isAccessMessage(strMessage))
	{
		KAccessMessage message;
		QString strError;
		if (!KAccessMessageCodec::decode(strMessage, &message, &strError))
		{
			if (m_sessionStateMachine.role() == ControlledSessionRole
				&& m_sessionStateMachine.isAwaitingApproval())
			{
				rejectIncomingAccess(QStringLiteral("invalid_request"), false);
			}
			else
			{
				finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
			}
			emit sessionError(strError);
			return;
		}
		handleAccessMessage(message);
		return;
	}

	if (m_sessionStateMachine.state() == NegotiatingSessionState
		|| m_sessionStateMachine.state() == ConnectedSessionState
		|| m_sessionStateMachine.state() == StreamingSessionState
		|| m_sessionStateMachine.state() == InterruptedSessionState)
	{
		m_spRemotePeerTransport->handleSignalingMessage(strMessage);
		return;
	}

	if (m_sessionStateMachine.isAwaitingApproval())
	{
		const bool bControlled = m_sessionStateMachine.role() == ControlledSessionRole;
		if (bControlled)
			rejectIncomingAccess(QStringLiteral("invalid_request"), false);
		else
			finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
		emit sessionError(QStringLiteral("对方版本不支持连接审批协议，请升级两端程序"));
	}
}

void KSessionCoordinator::handleAccessMessage(const KAccessMessage &message)
{
	if (!m_sessionStateMachine.isAwaitingApproval())
		return;

	if (m_sessionStateMachine.role() == ControlledSessionRole)
	{
		if (message.type == RejectedAccessMessageType
			&& message.strRequestId == m_strAccessRequestId)
		{
			rejectIncomingAccess(QStringLiteral("cancelled"), false);
			return;
		}
		if (message.type != RequestAccessMessageType || !m_strAccessRequestId.isEmpty())
			return;

		m_strAccessRequestId = message.strRequestId;
		m_strAccessDeviceName = message.strDeviceName;
		emit incomingAccessObserved(m_strAccessDeviceName, m_strAccessSourceAddress);
		if (!m_applicationSettings.bRemoteAccessEnabled)
		{
			rejectIncomingAccess(QStringLiteral("remote_access_disabled"), true);
			return;
		}
		if (m_applicationSettings.approvalMode == DenyRemoteApprovalMode)
		{
			rejectIncomingAccess(QStringLiteral("user_rejected"), true);
			return;
		}
		if (m_applicationSettings.approvalMode == AutoAcceptRemoteApprovalMode)
		{
			acceptIncomingAccess();
			return;
		}

		KAccessMessage pending;
		pending.type = PendingAccessMessageType;
		pending.strRequestId = m_strAccessRequestId;
		pending.nTimeoutSeconds = m_applicationSettings.nApprovalTimeoutSeconds;
		sendAccessMessage(pending);
		m_pApprovalTimer->start(m_applicationSettings.nApprovalTimeoutSeconds * 1000);
		const qint64 nExpiresAtMs = QDateTime::currentMSecsSinceEpoch()
			+ m_applicationSettings.nApprovalTimeoutSeconds * 1000;
		emit incomingAccessRequest(m_strAccessRequestId,
			m_strAccessDeviceName,
			m_strAccessSourceAddress,
			nExpiresAtMs);
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("access"),
			QStringLiteral("pending"),
			-1,
			QStringLiteral("requestId=%1 source=%2 timeoutSeconds=%3")
				.arg(m_strAccessRequestId)
				.arg(m_strAccessSourceAddress)
				.arg(m_applicationSettings.nApprovalTimeoutSeconds));
		return;
	}

	if (message.strRequestId != m_strAccessRequestId)
		return;
	if (message.type == PendingAccessMessageType)
	{
		m_pApprovalTimer->start(message.nTimeoutSeconds * 1000 + kApprovalResponseGraceMs);
		KSessionTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("access"), QStringLiteral("pending"), -1,
			QStringLiteral("requestId=%1 timeoutSeconds=%2")
				.arg(message.strRequestId)
				.arg(message.nTimeoutSeconds));
		return;
	}
	if (message.type == AcceptedAccessMessageType)
	{
		m_pApprovalTimer->stop();
		if (!m_sessionStateMachine.approveConnection())
			return;
		clearApprovalState(QStringLiteral("accepted"));
		emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		m_spRemotePeerTransport->createOffer();
		KSessionTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("access"), QStringLiteral("accepted"));
		return;
	}
	if (message.type == RejectedAccessMessageType)
	{
		const QString strReason = message.strReason;
		KSessionTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("access"), QStringLiteral("rejected"), -1,
			QStringLiteral("requestId=%1 reason=%2")
				.arg(message.strRequestId, strReason));
		finishSession(ConnectFailedSessionEndReason, strReason, false, false, false);
		emit sessionError(AccessRejectionError(strReason));
	}
}

void KSessionCoordinator::handleApprovalTimeout()
{
	if (!m_sessionStateMachine.isAwaitingApproval()
		|| m_nApprovalGeneration != m_sessionStateMachine.generation())
	{
		return;
	}
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("access"), QStringLiteral("timeout"), -1,
		QStringLiteral("requestId=%1 generation=%2")
			.arg(m_strAccessRequestId)
			.arg(m_nApprovalGeneration));

	if (m_sessionStateMachine.role() == ControlledSessionRole)
	{
		rejectIncomingAccess(QStringLiteral("timeout"), !m_strAccessRequestId.isEmpty());
		return;
	}

	finishSession(ConnectFailedSessionEndReason, QStringLiteral("approval_timeout"),
		false, false, false);
	emit sessionError(QStringLiteral("等待连接确认超时，或对方版本不兼容"));
}

void KSessionCoordinator::acceptIncomingAccess()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_strAccessRequestId.isEmpty()
		|| !m_sessionStateMachine.approveConnection())
	{
		return;
	}

	m_pApprovalTimer->stop();
	KAccessMessage accepted;
	accepted.type = AcceptedAccessMessageType;
	accepted.strRequestId = m_strAccessRequestId;
	sendAccessMessage(accepted);
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("access"), QStringLiteral("accepted"), -1,
		QStringLiteral("requestId=%1 source=%2")
			.arg(m_strAccessRequestId, m_strAccessSourceAddress));
	clearApprovalState(QStringLiteral("accepted"));
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::rejectIncomingAccess(const QString &strReason, bool bNotifyRemote)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| !m_sessionStateMachine.isAwaitingApproval())
	{
		return;
	}

	if (bNotifyRemote && !m_strAccessRequestId.isEmpty())
	{
		KAccessMessage rejected;
		rejected.type = RejectedAccessMessageType;
		rejected.strRequestId = m_strAccessRequestId;
		rejected.strReason = strReason;
		sendAccessMessage(rejected);
	}
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("access"), QStringLiteral("rejected"), -1,
		QStringLiteral("requestId=%1 source=%2 reason=%3")
			.arg(m_strAccessRequestId, m_strAccessSourceAddress, strReason));
	m_pApprovalTimer->stop();
	clearApprovalState(strReason);
	m_sessionStateMachine.rejectConnection();
	m_pSignaling->disconnectPeer();
	updateListeningAvailability(true, m_nListeningPort);
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::clearApprovalState(const QString &strReason)
{
	if (!m_strAccessRequestId.isEmpty()
		&& m_sessionStateMachine.role() == ControlledSessionRole)
	{
		emit incomingAccessRequestCleared(m_strAccessRequestId, strReason);
	}
	m_strAccessRequestId.clear();
	m_strAccessDeviceName.clear();
	m_strAccessSourceAddress.clear();
	m_nApprovalGeneration = 0;
}

void KSessionCoordinator::sendAccessMessage(const KAccessMessage &message)
{
	m_pSignaling->sendMessage(KAccessMessageCodec::encode(message));
}

void KSessionCoordinator::updateListeningAvailability(bool bAvailable, quint16 nPort)
{
	if (bAvailable && nPort != 0)
		m_nListeningPort = nPort;
	if (m_bListeningAvailable == bAvailable)
		return;
	m_bListeningAvailable = bAvailable;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("listening_availability"),
		bAvailable ? QStringLiteral("available") : QStringLiteral("unavailable"),
		-1,
		QStringLiteral("port=%1").arg(bAvailable ? m_nListeningPort : 0));
	emit listeningAvailabilityChanged(bAvailable, bAvailable ? m_nListeningPort : 0);
}

void KSessionCoordinator::handleSignalingConnectionLost()
{
	if (m_sessionStateMachine.isNegotiating())
	{
		finishSession(SignalingLostSessionEndReason,
			QString(),
			m_sessionStateMachine.shouldKeepListening(),
			false,
			true);
	}
}

void KSessionCoordinator::handlePeerConnectionInterrupted()
{
	if (!m_sessionStateMachine.interrupt())
		return;

	m_pInputInjector->releaseAllInputs();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_nDisconnectGraceGeneration = m_sessionStateMachine.generation();
	m_pDisconnectGraceTimer->start(kDisconnectGraceMs);
}

void KSessionCoordinator::handlePeerConnectionRestored()
{
	if (!m_sessionStateMachine.restore())
		return;

	m_pDisconnectGraceTimer->stop();
	m_nDisconnectGraceGeneration = 0;
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::handlePeerConnectionTerminated(const QString &strReason)
{
	if (!m_sessionStateMachine.canHandlePeerTermination())
		return;

	finishSession(PeerTerminatedSessionEndReason,
		strReason,
		m_sessionStateMachine.shouldKeepListening(),
		false,
		true);
}

void KSessionCoordinator::handleDisconnectGraceTimeout()
{
	if (!m_sessionStateMachine.isInterrupted()
		|| m_nDisconnectGraceGeneration != m_sessionStateMachine.generation())
		return;

	finishSession(DisconnectTimeoutSessionEndReason,
		QString(),
		m_sessionStateMachine.shouldKeepListening(),
		false,
		true);
}

void KSessionCoordinator::sendDeviceInfoMessage()
{
	KSessionMessage message;
	message.type = DeviceInfoSessionMessageType;
	if (m_spDeviceInfoProvider != nullptr)
		message.deviceInfo = m_spDeviceInfoProvider->deviceInfo();
	const QString strMessage = KSessionMessageCodec::encode(message);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("prepare"),
		KSessionMessageCodec::typeName(DeviceInfoSessionMessageType),
		strMessage.toUtf8().size(),
		QStringLiteral("wallpaper=%1 wallpaperBytes=%2 messageBytes=%3")
			.arg(message.deviceInfo.strWallpaperData.isEmpty() ? 0 : 1)
			.arg(message.deviceInfo.strWallpaperData.size())
			.arg(strMessage.toUtf8().size()));
	sendSessionMessage(message);
}
