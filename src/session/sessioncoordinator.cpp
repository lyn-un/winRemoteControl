#include "session/sessioncoordinator.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "core/input/inputinjectorinterface.h"
#include "core/protocol/accessmessage.h"
#include "core/protocol/protocolconstraints.h"
#include "core/protocol/webrtcsignalingmessage.h"
#include "core/session/deviceinfoprovider.h"
#include "core/transport/signalingtransport.h"
#include "input/inputinjector.h"

#include <QtCore/QTimer>
#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <utility>
#include <algorithm>

namespace
{
	constexpr int kReconnectTimeoutMs = 10000;
	constexpr int kInitialApprovalResponseTimeoutMs = 5000;
	constexpr int kApprovalResponseGraceMs = 5000;
	constexpr int kSessionCommandTimeoutMs = 1000;
	constexpr int kSessionCommandTimerIntervalMs = 100;
	constexpr int kMaximumSessionCommandAttempts = 2;
	constexpr int kMaximumRecentCommandResults = 128;

	static QString roleToString(KSessionRole role)
	{
		return KSessionStateMachine::roleName(role);
	}

	static bool shouldTraceInputMessage(const KInputMessage &message)
	{
		return message.bTrace
			|| ((message.type == KeyInputMessageType || message.type == TextInputMessageType)
				&& KLatencyTraceLogger::isEnabled());
	}

	static QString inputTraceExtra(const KInputMessage &message)
	{
		QString strExtra = QStringLiteral("seq=%1 type=%2")
			.arg(message.nSequence)
			.arg(KInputMessageCodec::typeName(message.type));
		if (message.type == KeyInputMessageType)
			strExtra += QStringLiteral(" pressed=%1").arg(message.bPressed ? 1 : 0);
		else if (message.type == TextInputMessageType)
			strExtra += QStringLiteral(" bytes=%1").arg(message.strText.toUtf8().size());
		return strExtra;
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
	, m_pReconnectTimer(new QTimer(this))
	, m_pApprovalTimer(new QTimer(this))
	, m_pStopWatchdogTimer(new QTimer(this))
	, m_pCapabilityTimer(new QTimer(this))
	, m_pSessionCommandTimer(new QTimer(this))
{
	Q_ASSERT(m_spRemotePeerTransport != nullptr);
	Q_ASSERT(m_pSignaling != nullptr);
	initializeProtocolRoutes();
	initializeSessionHandlers();
	m_pReconnectTimer->setSingleShot(true);
	m_pApprovalTimer->setSingleShot(true);
	m_pStopWatchdogTimer->setSingleShot(true);
	m_pCapabilityTimer->setSingleShot(true);
	m_pSessionCommandTimer->setInterval(kSessionCommandTimerIntervalMs);
	m_sessionCommandClock.start();
	connect(m_pReconnectTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleReconnectTimeout);
	connect(m_pApprovalTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleApprovalTimeout);
	connect(m_pStopWatchdogTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleStopWatchdog);
	connect(m_pCapabilityTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleCapabilityTimeout);
	connect(m_pSessionCommandTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleSessionCommandTimer);
	connect(m_pSignaling, &KSignalingTransport::stateChanged,
		this, &KSessionCoordinator::signalingChanged);
	connect(m_pSignaling, &KSignalingTransport::signalingError,
		this, [this](const QString &strMessage)
		{
			reportSessionError(SignalingSessionErrorDomain,
				ConnectionFailedSessionErrorCode, ConnectingSessionErrorStage,
				true, strMessage);
		});
	connect(m_pSignaling, &KSignalingTransport::outgoingConnectionEstablished,
		this, &KSessionCoordinator::handleOutgoingConnectionEstablished);
	connect(m_pSignaling, &KSignalingTransport::outgoingConnectionFailed,
		this, &KSessionCoordinator::handleOutgoingConnectionFailed);
	connect(m_pSignaling, &KSignalingTransport::incomingConnectionEstablished,
		this, &KSessionCoordinator::handleIncomingConnectionEstablished);
	connect(m_pSignaling, &KSignalingTransport::connectionLost,
		this, &KSessionCoordinator::handleSignalingConnectionLost);
	connect(m_pInputInjector, &KInputInjector::inputError,
		this, [this](const QString &strMessage)
		{
			reportSessionError(InputSessionErrorDomain,
				SendFailedSessionErrorCode, StreamingSessionErrorStage,
				false, strMessage);
		});
	connect(this, &KSessionController::captureShutdownFinished,
		this, &KSessionCoordinator::handleCaptureShutdownFinished);
	wirePeer();
}

KSessionCoordinator::~KSessionCoordinator()
{
	disconnectSession();
}

quint64 KSessionCoordinator::sessionGeneration() const
{
	return m_sessionStateMachine.generation();
}

bool KSessionCoordinator::isIdle() const
{
	return m_sessionStateMachine.state() == IdleSessionState;
}

void KSessionCoordinator::initializeProtocolRoutes()
{
	const KProtocolRouter::Guard controllerAwaitingApproval =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
		{
			return context.nRole == static_cast<int>(ControllerSessionRole)
				&& context.nState == static_cast<int>(AwaitingApprovalSessionState);
		};
	const KProtocolRouter::Guard controlledAwaitingApproval =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
		{
			return context.nRole == static_cast<int>(ControlledSessionRole)
				&& context.nState == static_cast<int>(AwaitingApprovalSessionState);
		};
	const KProtocolRouter::Guard eitherAwaitingApproval =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
		{
			return context.nState == static_cast<int>(AwaitingApprovalSessionState);
		};
	const auto acceptsWebRtcSignalingState = [](const KProtocolRouteContext &context)
	{
		return context.nState == static_cast<int>(NegotiatingSessionState)
			|| context.nState == static_cast<int>(ConnectedSessionState)
			|| context.nState == static_cast<int>(StreamingSessionState)
			|| context.nState == static_cast<int>(ReconnectingSessionState);
	};
	const KProtocolRouter::Guard controlledAcceptsOffer =
		[acceptsWebRtcSignalingState](const KProtocolEnvelope &,
			const KProtocolRouteContext &context)
		{
			return context.nRole == static_cast<int>(ControlledSessionRole)
				&& acceptsWebRtcSignalingState(context);
		};
	const KProtocolRouter::Guard controllerAcceptsAnswer =
		[acceptsWebRtcSignalingState](const KProtocolEnvelope &,
			const KProtocolRouteContext &context)
		{
			return context.nRole == static_cast<int>(ControllerSessionRole)
				&& acceptsWebRtcSignalingState(context);
		};
	const KProtocolRouter::Guard acceptsIce =
		[acceptsWebRtcSignalingState](const KProtocolEnvelope &,
			const KProtocolRouteContext &context)
		{
			return acceptsWebRtcSignalingState(context);
		};
	const KProtocolRouter::Handler accessHandler =
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return handleAccessEnvelope(envelope); };
	m_protocolRouter.registerHandler(SignalingProtocolChannel,
		KAccessMessageCodec::typeName(RequestAccessMessageType),
		controlledAwaitingApproval, accessHandler);
	for (KAccessMessageType type : { PendingAccessMessageType, AcceptedAccessMessageType })
	{
		m_protocolRouter.registerHandler(SignalingProtocolChannel,
			KAccessMessageCodec::typeName(type), controllerAwaitingApproval, accessHandler);
	}
	m_protocolRouter.registerHandler(SignalingProtocolChannel,
		KAccessMessageCodec::typeName(RejectedAccessMessageType),
		eitherAwaitingApproval, accessHandler);
	m_protocolRouter.registerHandler(SignalingProtocolChannel, QStringLiteral("busy"),
		controllerAwaitingApproval,
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return handleBusyEnvelope(envelope); });
	const KProtocolRouter::Handler signalingHandler =
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return handleWebRtcSignalingEnvelope(envelope); };
	m_protocolRouter.registerHandler(SignalingProtocolChannel,
		KWebRtcSignalingMessageCodec::typeName(OfferWebRtcSignalingMessageType),
		controlledAcceptsOffer, signalingHandler);
	m_protocolRouter.registerHandler(SignalingProtocolChannel,
		KWebRtcSignalingMessageCodec::typeName(AnswerWebRtcSignalingMessageType),
		controllerAcceptsAnswer, signalingHandler);
	m_protocolRouter.registerHandler(SignalingProtocolChannel,
		KWebRtcSignalingMessageCodec::typeName(IceCandidateWebRtcSignalingMessageType),
		acceptsIce, signalingHandler);
}

void KSessionCoordinator::initializeSessionHandlers()
{
	m_sessionHandlers.insert(DeviceInfoRequestSessionMessageType,
		[this](const KSessionMessage &message) { return handleDeviceInfoRequestMessage(message); });
	m_sessionHandlers.insert(DeviceInfoSessionMessageType,
		[this](const KSessionMessage &message) { return handleDeviceInfoMessage(message); });
	m_sessionHandlers.insert(StartStreamingSessionMessageType,
		[this](const KSessionMessage &message) { return handleStartStreamingMessage(message); });
	m_sessionHandlers.insert(StopStreamingSessionMessageType,
		[this](const KSessionMessage &message) { return handleStopStreamingMessage(message); });
	m_sessionHandlers.insert(EndSessionMessageType,
		[this](const KSessionMessage &message) { return handleEndSessionMessage(message); });
	m_sessionHandlers.insert(StreamConfigSessionMessageType,
		[this](const KSessionMessage &message) { return handleStreamConfigMessage(message); });
	m_sessionHandlers.insert(CapabilitiesSessionMessageType,
		[this](const KSessionMessage &message) { return handleCapabilitiesMessage(message); });
	m_sessionHandlers.insert(CapabilityRejectedSessionMessageType,
		[this](const KSessionMessage &message) { return handleCapabilityRejectedMessage(message); });
}

void KSessionCoordinator::publishSessionState()
{
	const KSessionState state = m_sessionStateMachine.state();
	emit sessionStateChanged(state);
}

void KSessionCoordinator::reportSessionError(KSessionErrorDomain domain,
	KSessionErrorCode code,
	KSessionErrorStage stage,
	bool bRetryable,
	const QString &strTechnicalMessage)
{
	KSessionError error;
	error.domain = domain;
	error.code = code;
	error.stage = stage;
	error.bRetryable = bRetryable;
	error.strTechnicalMessage = strTechnicalMessage;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_error"), KSessionError::codeName(code), -1,
		QStringLiteral("domain=%1 stage=%2 retryable=%3 technical=%4")
			.arg(KSessionError::domainName(domain), KSessionError::stageName(stage))
			.arg(bRetryable ? 1 : 0)
			.arg(strTechnicalMessage));
	emit sessionErrorOccurred(error);
}

void KSessionCoordinator::setRole(const QString &strRole)
{
	KSessionRole role;
	if (!KSessionStateMachine::roleFromString(strRole, &role))
		return;
	if (role != m_sessionStateMachine.role()
		&& m_sessionStateMachine.state() != IdleSessionState)
	{
		m_pendingRequestType = RolePendingRequest;
		m_pendingRole = role;
		finishSession(RoleChangedSessionEndReason, QString(), false, true, false);
		return;
	}
	if (m_sessionStateMachine.state() == IdleSessionState)
		m_sessionStateMachine.setRole(role);
	if (role == ControlledSessionRole)
	{
		m_strLastConnectionHost.clear();
		m_nLastConnectionPort = 0;
	}

	emit webRtcStateChanged(QStringLiteral("Role:%1")
		.arg(KSessionStateMachine::roleName(m_sessionStateMachine.role())));
}

void KSessionCoordinator::startSignalingServer(quint16 nPort)
{
	m_bSignalingConnected = false;
	if (!m_applicationSettings.bRemoteAccessEnabled)
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			RemoteAccessDisabledSessionErrorCode, ListeningSessionErrorStage,
			false, QStringLiteral("Remote access is disabled"));
		return;
	}
	if (m_sessionStateMachine.state() != IdleSessionState)
	{
		m_pendingRequestType = ListenPendingRequest;
		m_nPendingPort = nPort;
		finishSession(RestartListenerSessionEndReason, QString(), false, true, false);
		return;
	}
	QString strError;
	if (!initializePeer(ControlledSessionRole, &strError))
	{
		reportSessionError(WebRtcSessionErrorDomain,
			InitializationFailedSessionErrorCode, StartupSessionErrorStage,
			false, strError);
		return;
	}

	if (!m_pSignaling->startServer(nPort, &strError))
	{
		updateListeningAvailability(false);
		reportSessionError(SignalingSessionErrorDomain,
			ConnectionFailedSessionErrorCode, ListeningSessionErrorStage,
			true, strError);
		return;
	}

	m_sessionStateMachine.beginListening();
	updateListeningAvailability(true, nPort);
	publishSessionState();
}

void KSessionCoordinator::connectSignaling(const QString &strHost, quint16 nPort)
{
	const QString strTargetHost = strHost;
	if (strTargetHost.isEmpty() || nPort == 0)
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			InvalidArgumentSessionErrorCode, ConnectingSessionErrorStage,
			false, QStringLiteral("Invalid controlled endpoint"));
		return;
	}
	m_strLastConnectionHost = strTargetHost;
	m_nLastConnectionPort = nPort;
	m_bSignalingConnected = false;
	if (m_sessionStateMachine.state() != IdleSessionState)
	{
		m_pendingRequestType = ConnectPendingRequest;
		m_strPendingHost = strTargetHost;
		m_nPendingPort = nPort;
		finishSession(NewConnectionSessionEndReason, QString(), false, true, false);
		return;
	}
	m_pSignaling->stop();
	QString strError;
	if (!initializePeer(ControllerSessionRole, &strError))
	{
		reportSessionError(WebRtcSessionErrorDomain,
			InitializationFailedSessionErrorCode, StartupSessionErrorStage,
			false, strError);
		return;
	}

	m_sessionStateMachine.beginConnecting();
	publishSessionState();
	m_pSignaling->connectToHost(strTargetHost, nPort);
}

void KSessionCoordinator::retryLastConnection()
{
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| m_sessionStateMachine.state() != IdleSessionState
		|| m_strLastConnectionHost.isEmpty()
		|| m_nLastConnectionPort == 0)
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			InvalidArgumentSessionErrorCode, ConnectingSessionErrorStage,
			false, QStringLiteral("No last connection endpoint"));
		return;
	}

	const QString strHost = m_strLastConnectionHost;
	const quint16 nPort = m_nLastConnectionPort;
	connectSignaling(strHost, nPort);
	KSessionTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("session_reconnect_requested"),
		QStringLiteral("retry"),
		-1,
		QStringLiteral("generation=%1").arg(m_sessionStateMachine.generation()));
}

void KSessionCoordinator::disconnectSession()
{
	m_pendingRequestType = NoPendingRequest;
	m_strPendingHost.clear();
	m_nPendingPort = 0;
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
	KSessionMessage message;
	message.type = StartStreamingSessionMessageType;
	message.streamConfig = constrainedStreamConfig(config);
	message.bHasStreamConfig = true;
	sendSessionMessage(message);
	m_sessionStateMachine.beginStreaming();
	publishSessionState();
}

void KSessionCoordinator::leaveRemoteDesktop()
{
	if (!m_sessionStateMachine.canLeaveRemoteDesktop())
		return;
	KSessionMessage message;
	message.type = StopStreamingSessionMessageType;
	sendSessionMessage(message);
	m_sessionStateMachine.stopStreaming();
	emit stopCaptureRequested(m_sessionStateMachine.generation());
	m_bCaptureActive = false;
	publishSessionState();
}

void KSessionCoordinator::startStreaming()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole)
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			InvalidStateSessionErrorCode, StreamingSessionErrorStage,
			false, QStringLiteral("Only the controlled role can start streaming"));
		return;
	}
	if (!m_bSessionChannelOpen || !m_sessionStateMachine.canStartControlledStreaming())
		return;

	emit startCaptureRequested(m_sessionStateMachine.generation());
	m_bCaptureActive = true;
	m_sessionStateMachine.beginStreaming();
	publishSessionState();
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

void KSessionCoordinator::sendClipboardMessage(const KClipboardMessage &message)
{
	if (!m_sessionStateMachine.canSyncClipboard() || !m_bClipboardChannelOpen)
		return;
	m_spRemotePeerTransport->sendClipboardMessage(message);
}

QString KSessionCoordinator::sendSessionMessage(KSessionMessage message)
{
	if (KSessionMessageCodec::isCommand(message.type) && message.strRequestId.isEmpty())
		message.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	if (!transmitSessionMessage(message))
		return QString();
	if (!KSessionMessageCodec::isCommand(message.type))
		return message.strRequestId;

	KPendingSessionCommand pending;
	pending.message = message;
	pending.nSentMs = m_sessionCommandClock.elapsed();
	pending.nAttempts = 1;
	pending.nGeneration = m_sessionStateMachine.generation();
	m_pendingSessionCommands.insert(message.strRequestId, pending);
	if (!m_pSessionCommandTimer->isActive())
		m_pSessionCommandTimer->start();
	return message.strRequestId;
}

bool KSessionCoordinator::transmitSessionMessage(const KSessionMessage &message)
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
		return false;
	}

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("send"),
		strType,
		strMessage.toUtf8().size());
	if (m_spRemotePeerTransport->sendSessionMessage(message))
		return true;

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("send_failed"), strType, strMessage.toUtf8().size(),
		QStringLiteral("requestId=%1").arg(message.strRequestId));
	return false;
}

void KSessionCoordinator::handleSessionCommandTimer()
{
	const qint64 nNowMs = m_sessionCommandClock.elapsed();
	const QStringList requestIds = m_pendingSessionCommands.keys();
	for (const QString &strRequestId : requestIds)
	{
		auto iterator = m_pendingSessionCommands.find(strRequestId);
		if (iterator == m_pendingSessionCommands.end())
			continue;
		KPendingSessionCommand &pending = iterator.value();
		if (pending.nGeneration != m_sessionStateMachine.generation())
		{
			m_pendingSessionCommands.erase(iterator);
			continue;
		}
		if (nNowMs - pending.nSentMs < kSessionCommandTimeoutMs)
			continue;
		if (pending.nAttempts < kMaximumSessionCommandAttempts
			&& transmitSessionMessage(pending.message))
		{
			++pending.nAttempts;
			pending.nSentMs = nNowMs;
			KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
				QStringLiteral("session_command_retry"),
				KSessionMessageCodec::typeName(pending.message.type), -1,
				QStringLiteral("requestId=%1 attempt=%2 generation=%3")
					.arg(strRequestId)
					.arg(pending.nAttempts)
					.arg(pending.nGeneration));
			continue;
		}

		const KSessionMessageType type = pending.message.type;
		m_pendingSessionCommands.erase(iterator);
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("session_command_timeout"),
			KSessionMessageCodec::typeName(type), -1,
			QStringLiteral("requestId=%1 generation=%2")
				.arg(strRequestId)
				.arg(m_sessionStateMachine.generation()));
		if (strRequestId == m_strPendingEndCommandId)
		{
			m_strPendingEndCommandId.clear();
			continueStoppingTeardown();
			break;
		}
		reportSessionError(ProtocolSessionErrorDomain,
			CommandTimeoutSessionErrorCode, ConnectedSessionErrorStage,
			false, QStringLiteral("Session command timed out: %1")
				.arg(KSessionMessageCodec::typeName(type)));
		finishSession(DisconnectTimeoutSessionEndReason,
			QStringLiteral("session_command_timeout"),
			m_sessionStateMachine.shouldKeepListening(), false, false);
		break;
	}
	if (m_pendingSessionCommands.isEmpty())
		m_pSessionCommandTimer->stop();
}

void KSessionCoordinator::handleCommandResultMessage(const KSessionMessage &message)
{
	auto iterator = m_pendingSessionCommands.find(message.strRequestId);
	if (iterator == m_pendingSessionCommands.end()
		|| iterator->nGeneration != m_sessionStateMachine.generation())
	{
		return;
	}
	const KSessionMessageType commandType = iterator->message.type;
	m_pendingSessionCommands.erase(iterator);
	if (m_pendingSessionCommands.isEmpty())
		m_pSessionCommandTimer->stop();
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_command_result"),
		KSessionMessageCodec::typeName(commandType), -1,
		QStringLiteral("requestId=%1 success=%2 errorCode=%3")
			.arg(message.strRequestId)
			.arg(message.bSuccess ? 1 : 0)
			.arg(message.strErrorCode));
	if (message.strRequestId == m_strPendingEndCommandId)
	{
		m_strPendingEndCommandId.clear();
		continueStoppingTeardown();
		return;
	}
	if (!message.bSuccess)
	{
		reportSessionError(ProtocolSessionErrorDomain,
			InvalidStateSessionErrorCode, ConnectedSessionErrorStage,
			false, QStringLiteral("Remote rejected session command: %1")
				.arg(message.strErrorCode));
	}
}

void KSessionCoordinator::sendCommandResult(const QString &strRequestId,
	const KProtocolHandlerResult &handlerResult)
{
	KSessionMessage result;
	result.type = CommandResultSessionMessageType;
	result.strRequestId = strRequestId;
	result.bSuccess = handlerResult.status == ProtocolHandlerSucceeded;
	if (!result.bSuccess)
	{
		if (handlerResult.status == ProtocolHandlerInvalidState)
			result.strErrorCode = QStringLiteral("invalid_state");
		else if (handlerResult.status == ProtocolHandlerPermissionDenied)
			result.strErrorCode = QStringLiteral("permission_denied");
		else
			result.strErrorCode = QStringLiteral("execution_failed");
	}
	rememberCommandResult(result);
	transmitSessionMessage(result);
}

void KSessionCoordinator::rememberCommandResult(const KSessionMessage &message)
{
	if (m_recentCommandResults.contains(message.strRequestId))
		return;
	m_recentCommandResults.insert(message.strRequestId, message);
	m_recentCommandResultIds.enqueue(message.strRequestId);
	while (m_recentCommandResultIds.size() > kMaximumRecentCommandResults)
		m_recentCommandResults.remove(m_recentCommandResultIds.dequeue());
}

void KSessionCoordinator::clearSessionCommands()
{
	m_pSessionCommandTimer->stop();
	m_pendingSessionCommands.clear();
	m_recentCommandResults.clear();
	m_recentCommandResultIds.clear();
	m_strPendingEndCommandId.clear();
}

void KSessionCoordinator::sendStreamConfig(const KStreamConfig &config)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	KSessionMessage message;
	message.type = StreamConfigSessionMessageType;
	message.streamConfig = constrainedStreamConfig(config);
	sendSessionMessage(message);
}

KStreamConfig KSessionCoordinator::constrainedStreamConfig(const KStreamConfig &config) const
{
	KStreamConfig result = config;
	if (m_negotiatedCapabilities.bValid)
	{
		if (result.nWidth > 0)
			result.nWidth = std::min(result.nWidth,
				m_negotiatedCapabilities.nMaximumWidth);
		if (result.nHeight > 0)
			result.nHeight = std::min(result.nHeight,
				m_negotiatedCapabilities.nMaximumHeight);
		result.nFps = std::min(result.nFps,
			m_negotiatedCapabilities.nMaximumFps);
		result.nBitrateKbps = std::min(result.nBitrateKbps,
			m_negotiatedCapabilities.nMaximumBitrateKbps);
	}
	return result;
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
	const bool bRecovering = m_sessionStateMachine.isReconnecting();
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
	publishSessionState();
	KSessionTraceLogger::write(roleToString(role),
		QStringLiteral("session_end"),
		strReason,
		-1,
		QStringLiteral("generation=%1 keepListening=%2 notifyRemote=%3")
			.arg(nGeneration)
			.arg(bKeepListening ? 1 : 0)
			.arg(bNotifyRemote ? 1 : 0));
	m_bStopKeepListening = bKeepListening;
	m_bStopReportError = bReportError;
	m_bStopRecovering = bRecovering;
	m_stopRole = role;
	m_strStopReason = strReason;
	m_nStoppingGeneration = nGeneration;
	m_bStopTeardownStarted = false;
	m_pInputInjector->releaseAllInputs();
	resetInputTraceState();

	if (bNotifyRemote && m_bSessionChannelOpen)
	{
		KSessionMessage message;
		message.type = EndSessionMessageType;
		message.strReason = strReason;
		m_strPendingEndCommandId = sendSessionMessage(message);
		if (!m_strPendingEndCommandId.isEmpty())
			return;
	}
	continueStoppingTeardown();
}

void KSessionCoordinator::continueStoppingTeardown()
{
	if (m_bStopTeardownStarted || !m_sessionStateMachine.isStopping())
		return;
	m_bStopTeardownStarted = true;

	const bool bRecovering = m_bStopRecovering;
	const KSessionRole role = m_stopRole;
	const QString strReason = m_strStopReason;
	const quint64 nGeneration = m_nStoppingGeneration;

	if (bRecovering)
	{
		KSessionTraceLogger::write(roleToString(role),
			QStringLiteral("session_recovery_failed"),
			strReason,
			-1,
			QStringLiteral("costMs=%1")
				.arg(m_reconnectElapsedTimer.isValid() ? m_reconnectElapsedTimer.elapsed() : -1));
	}
	m_pReconnectTimer->stop();
	m_pApprovalTimer->stop();
	m_pCapabilityTimer->stop();
	m_nReconnectGeneration = 0;
	m_reconnectElapsedTimer.invalidate();
	m_bSignalingConnected = false;
	clearApprovalState(strReason);
	m_bDeviceInfoRequested = false;
	m_nInvalidSignalingMessages = 0;
	m_bInputChannelOpen = false;
	m_bClipboardChannelOpen = false;
	m_bSessionChannelOpen = false;
	m_bCapabilitiesReceived = false;
	m_negotiatedCapabilities = KNegotiatedCapabilities();
	emit sessionCapabilitiesChanged(m_negotiatedCapabilities);
	clearSessionCommands();
	m_bCaptureShutdownPending = m_bCaptureActive;
	m_bPeerShutdownPending = true;
	m_pStopWatchdogTimer->start(3000);
	if (m_bCaptureActive)
		emit stopCaptureRequested(nGeneration);
	m_bCaptureActive = false;
	emit sessionChannelChanged(false);
	emit networkStatsReady(KNetworkStats());
	m_spRemotePeerTransport->requestShutdown(nGeneration);
}

void KSessionCoordinator::handleCaptureShutdownFinished(quint64 nGeneration)
{
	if (nGeneration != m_nStoppingGeneration || !m_sessionStateMachine.isStopping())
		return;
	m_bCaptureShutdownPending = false;
	tryFinishStopping();
}

void KSessionCoordinator::handlePeerShutdownFinished(quint64 nGeneration)
{
	if (nGeneration != m_nStoppingGeneration || !m_sessionStateMachine.isStopping())
		return;
	m_bPeerShutdownPending = false;
	tryFinishStopping();
}

void KSessionCoordinator::tryFinishStopping()
{
	if (m_bCaptureShutdownPending || m_bPeerShutdownPending)
		return;
	finishStopping();
}

void KSessionCoordinator::handleStopWatchdog()
{
	if (!m_sessionStateMachine.isStopping())
		return;
	KSessionTraceLogger::write(roleToString(m_stopRole),
		QStringLiteral("session_stop_watchdog"),
		QStringLiteral("timeout"),
		-1,
		QStringLiteral("generation=%1 capturePending=%2 peerPending=%3")
			.arg(m_nStoppingGeneration)
			.arg(m_bCaptureShutdownPending ? 1 : 0)
			.arg(m_bPeerShutdownPending ? 1 : 0));
}

void KSessionCoordinator::finishStopping()
{
	m_pStopWatchdogTimer->stop();
	const bool bKeepListening = m_bStopKeepListening;
	const bool bReportError = m_bStopReportError;
	const bool bRecovering = m_bStopRecovering;
	const KSessionRole role = m_stopRole;
	const QString strReason = m_strStopReason;
	m_bCaptureShutdownPending = false;
	m_bPeerShutdownPending = false;
	m_bStopTeardownStarted = false;

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
			publishSessionState();
		}
		else
		{
			m_pSignaling->stop();
			m_sessionStateMachine.finish(false);
			publishSessionState();
			reportSessionError(WebRtcSessionErrorDomain,
				InitializationFailedSessionErrorCode, ShutdownSessionErrorStage,
				false, strError);
		}
	}
	else
	{
		m_pSignaling->stop();
		m_sessionStateMachine.finish(false);
		publishSessionState();
	}

	if (bReportError)
	{
		reportSessionError(WebRtcSessionErrorDomain,
			bRecovering
				? RecoveryFailedSessionErrorCode : ConnectionLostSessionErrorCode,
			bRecovering ? RecoverySessionErrorStage : ConnectedSessionErrorStage,
			true, QStringLiteral("Remote session ended: %1").arg(strReason));
	}
	else if (bListening)
	{
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("session_ready"),
			QStringLiteral("listening_after_end"));
	}
	executePendingRequest();
}

void KSessionCoordinator::executePendingRequest()
{
	const PendingRequestType requestType = m_pendingRequestType;
	const QString strHost = m_strPendingHost;
	const quint16 nPort = m_nPendingPort;
	const KSessionRole role = m_pendingRole;
	m_pendingRequestType = NoPendingRequest;
	m_strPendingHost.clear();
	m_nPendingPort = 0;
	if (requestType == ListenPendingRequest)
		startSignalingServer(nPort);
	else if (requestType == ConnectPendingRequest)
		connectSignaling(strHost, nPort);
	else if (requestType == RolePendingRequest)
		setRole(KSessionStateMachine::roleName(role));
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
	++m_nActivePeerGeneration;
	return m_spRemotePeerTransport->initialize(role, m_nActivePeerGeneration, pErrorMessage);
}

void KSessionCoordinator::wirePeer()
{
	KRemotePeerTransport *pTransport = m_spRemotePeerTransport.get();
	connect(pTransport, &KRemotePeerTransport::shutdownFinished,
		this, &KSessionCoordinator::handlePeerShutdownFinished);
	connect(pTransport, &KRemotePeerTransport::signalingMessageReady,
		this, [this](quint64 nGeneration, const QString &strMessage)
		{
			if (nGeneration == m_nActivePeerGeneration)
				m_pSignaling->sendMessage(strMessage);
		});
	connect(m_pSignaling, &KSignalingTransport::messageReceived,
		this, &KSessionCoordinator::handleSignalingMessage);
	connect(pTransport, &KRemotePeerTransport::stateChanged,
		this, [this](quint64 nGeneration, const QString &strState)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
				QStringLiteral("webrtc_state"), strState);
		});
	connect(pTransport, &KRemotePeerTransport::transportError,
		this, [this](quint64 nGeneration, const QString &strMessage)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			reportSessionError(WebRtcSessionErrorDomain,
				SendFailedSessionErrorCode,
				m_sessionStateMachine.isReconnecting()
					? RecoverySessionErrorStage : NegotiationSessionErrorStage,
				true, strMessage);
		});
	connect(pTransport, &KRemotePeerTransport::remoteFrameReady,
		this, [this](quint64 nGeneration, const KDecodedVideoFrame &frame)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleRemoteFrame(frame);
		}, Qt::DirectConnection);
	connect(pTransport, &KRemotePeerTransport::networkStatsReady,
		this, [this](quint64 nGeneration, const KNetworkStats &stats)
		{
			if (nGeneration == m_nActivePeerGeneration)
				emit networkStatsReady(stats);
		});
	connect(pTransport, &KRemotePeerTransport::inputMessageReceived,
		this, [this](quint64 nGeneration, const KInputMessage &message)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleInputMessage(message);
		});
	connect(m_pInputInjector, &KInputInjector::inputInjected,
		this, &KSessionCoordinator::handleInputInjected);
	connect(pTransport, &KRemotePeerTransport::inputChannelChanged,
		this, [this](quint64 nGeneration, bool bOpen)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleInputChannelChanged(bOpen);
		});
	connect(pTransport, &KRemotePeerTransport::clipboardMessageReceived,
		this, [this](quint64 nGeneration, const KClipboardMessage &message)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleClipboardMessage(message);
		});
	connect(pTransport, &KRemotePeerTransport::clipboardChannelChanged,
		this, [this](quint64 nGeneration, bool bOpen)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleClipboardChannelChanged(bOpen);
		});
	connect(pTransport, &KRemotePeerTransport::sessionMessageReceived,
		this, [this](quint64 nGeneration, const KSessionMessage &message)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleSessionMessage(message);
		});
	connect(pTransport, &KRemotePeerTransport::sessionChannelChanged,
		this, [this](quint64 nGeneration, bool bOpen)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handleSessionChannelChanged(bOpen);
		});
	connect(pTransport, &KRemotePeerTransport::sessionChannelChanged,
		this, [this](quint64 nGeneration, bool bOpen)
		{
			if (nGeneration == m_nActivePeerGeneration)
				emit sessionChannelChanged(bOpen);
		});
	connect(pTransport, &KRemotePeerTransport::connectionInterrupted,
		this, [this](quint64 nGeneration)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handlePeerConnectionInterrupted();
		});
	connect(pTransport, &KRemotePeerTransport::connectionRestored,
		this, [this](quint64 nGeneration)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handlePeerConnectionRestored();
		});
	connect(pTransport, &KRemotePeerTransport::connectionTerminated,
		this, [this](quint64 nGeneration, const QString &strReason)
		{
			if (nGeneration == m_nActivePeerGeneration)
				handlePeerConnectionTerminated(strReason);
		});
	connect(pTransport, &KRemotePeerTransport::inputBackpressureOverflow,
		this, [this](quint64 nGeneration)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			if (!m_sessionStateMachine.canHandlePeerTermination())
				return;
			finishSession(PeerTerminatedSessionEndReason,
				QStringLiteral("input_backpressure_overflow"),
				m_sessionStateMachine.shouldKeepListening(), false, false);
			reportSessionError(InputSessionErrorDomain,
				InputBackpressureOverflowSessionErrorCode,
				StreamingSessionErrorStage, false,
				QStringLiteral("Reliable input queue overflow"));
		});
	connect(pTransport, &KRemotePeerTransport::protocolViolation,
		this, [this](quint64 nGeneration, const QString &, const QString &strTechnicalMessage)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			if (!m_sessionStateMachine.canHandlePeerTermination())
				return;
			finishSession(ConnectFailedSessionEndReason,
				QStringLiteral("protocol_violation"),
				m_sessionStateMachine.shouldKeepListening(), false, false);
			reportSessionError(ProtocolSessionErrorDomain,
				ProtocolViolationSessionErrorCode,
				ConnectedSessionErrorStage, false, strTechnicalMessage);
		});
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

void KSessionCoordinator::handleClipboardMessage(const KClipboardMessage &message)
{
	const bool bTextMessage = message.type == TextClipboardMessageType;
	const QString strKind = bTextMessage
		? QStringLiteral("text")
		: message.type == ReadyClipboardMessageType
			? QStringLiteral("ready")
			: QStringLiteral("sync_state");
	const int nPayloadBytes = bTextMessage ? message.strText.toUtf8().size() : -1;
	if (!m_sessionStateMachine.canSyncClipboard() || !m_bClipboardChannelOpen)
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("clipboard_drop"),
			QStringLiteral("session_inactive"),
			nPayloadBytes,
			QStringLiteral("messageId=%1").arg(message.strMessageId));
		return;
	}

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("clipboard_recv"),
		strKind,
		nPayloadBytes,
		QStringLiteral("messageId=%1").arg(message.strMessageId));
	emit clipboardMessageReceived(message);
}

void KSessionCoordinator::handleClipboardChannelChanged(bool bOpen)
{
	m_bClipboardChannelOpen = bOpen;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("channel"),
		QStringLiteral("clipboard"),
		-1,
		QStringLiteral("open=%1").arg(bOpen ? 1 : 0));
	emit clipboardChannelChanged(bOpen && m_negotiatedCapabilities.bClipboardText);
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
		m_pCapabilityTimer->stop();
		m_bCapabilitiesReceived = false;
		m_bDeviceInfoRequested = false;
		if (m_sessionStateMachine.isStopping() && !m_strPendingEndCommandId.isEmpty())
		{
			m_strPendingEndCommandId.clear();
			continueStoppingTeardown();
		}
		else if (bWasOpen && !m_sessionStateMachine.isStopping())
		{
			finishSession(SessionChannelClosedSessionEndReason,
				QString(),
				m_sessionStateMachine.shouldKeepListening(),
				false,
				true);
		}
		return;
	}
	m_bCapabilitiesReceived = false;
	m_negotiatedCapabilities = KNegotiatedCapabilities();
	m_pCapabilityTimer->start(3000);
	KSessionMessage capabilitiesMessage;
	capabilitiesMessage.type = CapabilitiesSessionMessageType;
	capabilitiesMessage.capabilities = localCapabilities();
	sendSessionMessage(capabilitiesMessage);
}

void KSessionCoordinator::completeCapabilityNegotiation(
	const KNegotiatedCapabilities &capabilities)
{
	if (m_bCapabilitiesReceived || !m_bSessionChannelOpen)
		return;
	m_bCapabilitiesReceived = true;
	m_pCapabilityTimer->stop();
	m_negotiatedCapabilities = capabilities;
	if (!m_sessionStateMachine.markConnected())
		return;
	publishSessionState();
	emit sessionCapabilitiesChanged(capabilities);
	emit clipboardChannelChanged(m_bClipboardChannelOpen && capabilities.bClipboardText);
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

KSessionCapabilities KSessionCoordinator::localCapabilities() const
{
	KSessionCapabilities capabilities;
	capabilities.supportedCodecs = { QStringLiteral("h264") };
	capabilities.supportedChannels = {
		QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input"),
		QStringLiteral("clipboard")
	};
	capabilities.nMaximumWidth = KProtocolConstraints::kMaximumStreamWidth;
	capabilities.nMaximumHeight = KProtocolConstraints::kMaximumStreamHeight;
	capabilities.nMaximumFps = KProtocolConstraints::kMaximumStreamFps;
	capabilities.nMaximumBitrateKbps = KProtocolConstraints::kMaximumStreamBitrateKbps;
	KMonitorCapability monitor;
	monitor.strId = QStringLiteral("default");
	monitor.nWidth = capabilities.nMaximumWidth;
	monitor.nHeight = capabilities.nMaximumHeight;
	monitor.bPrimary = true;
	capabilities.monitorList.append(monitor);
	return capabilities;
}

KProtocolHandlerResult KSessionCoordinator::handleCapabilitiesMessage(
	const KSessionMessage &message)
{
	if (!m_bSessionChannelOpen || m_sessionStateMachine.state() != NegotiatingSessionState)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Capabilities received outside negotiation"));
	}
	KNegotiatedCapabilities negotiated;
	QString strError;
	if (!KSessionMessageCodec::negotiate(localCapabilities(), message.capabilities,
			&negotiated, &strError))
	{
		KSessionMessage rejection;
		rejection.type = CapabilityRejectedSessionMessageType;
		rejection.strReason = QStringLiteral("incompatible_capabilities");
		sendSessionMessage(rejection);
		finishSession(ConnectFailedSessionEndReason, QStringLiteral("incompatible_protocol"),
			m_sessionStateMachine.shouldKeepListening(), false, false);
		reportSessionError(ProtocolSessionErrorDomain,
			IncompatibleProtocolSessionErrorCode, NegotiationSessionErrorStage,
			false, strError);
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed, strError);
	}
	completeCapabilityNegotiation(negotiated);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleCapabilityRejectedMessage(
	const KSessionMessage &message)
{
	if (m_sessionStateMachine.state() != NegotiatingSessionState)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Capability rejection received outside negotiation"));
	}
	finishSession(ConnectFailedSessionEndReason, QStringLiteral("incompatible_protocol"),
		m_sessionStateMachine.shouldKeepListening(), false, false);
	reportSessionError(ProtocolSessionErrorDomain,
		IncompatibleProtocolSessionErrorCode, NegotiationSessionErrorStage,
		false, message.strReason);
	return KProtocolHandlerResult::success();
}

void KSessionCoordinator::handleCapabilityTimeout()
{
	if (m_sessionStateMachine.state() != NegotiatingSessionState || m_bCapabilitiesReceived)
		return;
	finishSession(ConnectFailedSessionEndReason, QStringLiteral("capability_timeout"),
		m_sessionStateMachine.shouldKeepListening(), false, false);
	reportSessionError(ProtocolSessionErrorDomain,
		IncompatibleProtocolSessionErrorCode, NegotiationSessionErrorStage,
		false, QStringLiteral("The peer does not support capability negotiation; upgrade both clients"));
}

void KSessionCoordinator::handleSessionMessage(const KSessionMessage &message)
{
	if (message.type == CommandResultSessionMessageType)
	{
		handleCommandResultMessage(message);
		return;
	}
	if (!m_bCapabilitiesReceived
		&& message.type != CapabilitiesSessionMessageType
		&& message.type != CapabilityRejectedSessionMessageType
		&& message.type != EndSessionMessageType)
	{
		KSessionMessage rejection;
		rejection.type = CapabilityRejectedSessionMessageType;
		rejection.strReason = QStringLiteral("capability_negotiation_required");
		sendSessionMessage(rejection);
		finishSession(ConnectFailedSessionEndReason, QStringLiteral("incompatible_protocol"),
			m_sessionStateMachine.shouldKeepListening(), false, false);
		reportSessionError(ProtocolSessionErrorDomain,
			IncompatibleProtocolSessionErrorCode, NegotiationSessionErrorStage,
			false, QStringLiteral("The peer sent session data before capability negotiation"));
		return;
	}
	const QString strMessage = KSessionMessageCodec::encode(message);
	const QString strType = KSessionMessageCodec::typeName(message.type);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("handle"),
		strType,
		strMessage.toUtf8().size());
	if (KSessionMessageCodec::isCommand(message.type))
	{
		const auto cached = m_recentCommandResults.constFind(message.strRequestId);
		if (cached != m_recentCommandResults.constEnd())
		{
			transmitSessionMessage(cached.value());
			return;
		}
	}

	KProtocolHandlerResult result = KProtocolHandlerResult::failure(
		ProtocolHandlerExecutionFailed, QStringLiteral("No session message handler"));
	const auto iterator = m_sessionHandlers.constFind(static_cast<int>(message.type));
	if (iterator != m_sessionHandlers.constEnd())
		result = iterator.value()(message);
	if (KSessionMessageCodec::isCommand(message.type))
	{
		sendCommandResult(message.strRequestId, result);
		if (message.type == EndSessionMessageType && result.status == ProtocolHandlerSucceeded)
		{
			finishSession(RemoteStopSessionEndReason,
				message.strReason,
				m_sessionStateMachine.shouldKeepListening(),
				false,
				m_sessionStateMachine.role() == ControllerSessionRole);
		}
	}
}

KProtocolHandlerResult KSessionCoordinator::handleDeviceInfoRequestMessage(
	const KSessionMessage &)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerPermissionDenied,
			QStringLiteral("Only controlled role can provide device information"));
	}
	sendDeviceInfoMessage();
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleDeviceInfoMessage(
	const KSessionMessage &message)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerPermissionDenied,
			QStringLiteral("Only controller role can receive device information"));
	}
	emit remoteDeviceInfoChanged(message.deviceInfo.strComputerName,
		message.deviceInfo.strWallpaperMime,
		message.deviceInfo.strWallpaperData,
		message.deviceInfo.nScreenWidth,
		message.deviceInfo.nScreenHeight);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleStartStreamingMessage(
	const KSessionMessage &message)
{
	if (!m_sessionStateMachine.canStartControlledStreaming())
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Cannot start controlled streaming in the current state"));
	}
	if (message.bHasStreamConfig)
	{
		m_spRemotePeerTransport->setStreamConfig(message.streamConfig);
		emit streamConfigChanged(message.streamConfig);
	}
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("emit"), QStringLiteral("startCaptureRequested"));
	emit startCaptureRequested(m_sessionStateMachine.generation());
	m_bCaptureActive = true;
	m_sessionStateMachine.beginStreaming();
	publishSessionState();
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleStopStreamingMessage(
	const KSessionMessage &)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.state() != StreamingSessionState)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Cannot stop controlled streaming in the current state"));
	}
	m_sessionStateMachine.stopStreaming();
	m_pInputInjector->releaseAllInputs();
	resetInputTraceState();
	emit stopCaptureRequested(m_sessionStateMachine.generation());
	m_bCaptureActive = false;
	publishSessionState();
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleEndSessionMessage(
	const KSessionMessage &)
{
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleStreamConfigMessage(
	const KSessionMessage &message)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerPermissionDenied,
			QStringLiteral("Only controlled role can apply stream configuration"));
	}
	m_spRemotePeerTransport->setStreamConfig(message.streamConfig);
	emit streamConfigChanged(message.streamConfig);
	return KProtocolHandlerResult::success();
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
	m_bSignalingConnected = true;

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
	publishSessionState();
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
	m_bSignalingConnected = false;
	if (!m_sessionStateMachine.isConnecting())
		return;

	finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
	reportSessionError(SignalingSessionErrorDomain,
		ConnectionFailedSessionErrorCode, ConnectingSessionErrorStage,
		true, strMessage);
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
	m_bSignalingConnected = true;
	m_strAccessSourceAddress = strSourceAddress;
	m_nApprovalGeneration = m_sessionStateMachine.generation();
	m_pApprovalTimer->start(kInitialApprovalResponseTimeoutMs);
	updateListeningAvailability(false);
	publishSessionState();
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
	KProtocolRouteContext context;
	context.nRole = static_cast<int>(m_sessionStateMachine.role());
	context.nState = static_cast<int>(m_sessionStateMachine.state());
	context.nGeneration = m_sessionStateMachine.generation();
	const KProtocolRouteResult result = m_protocolRouter.route(
		SignalingProtocolChannel, strMessage, context);
	if (result.status == HandledProtocolRouteStatus)
	{
		m_nInvalidSignalingMessages = 0;
		return;
	}
	handleInvalidSignalingMessage(result.status, result.strError);
}

KProtocolHandlerResult KSessionCoordinator::handleAccessEnvelope(
	const KProtocolEnvelope &envelope)
{
	KAccessMessage message;
	QString strError;
	if (!KAccessMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	handleAccessMessage(message);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleWebRtcSignalingEnvelope(
	const KProtocolEnvelope &envelope)
{
	KWebRtcSignalingMessage message;
	QString strError;
	if (!KWebRtcSignalingMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	m_spRemotePeerTransport->handleSignalingMessage(message);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleBusyEnvelope(const KProtocolEnvelope &)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Busy response received by controlled peer"));
	}
	finishSession(ConnectFailedSessionEndReason, QStringLiteral("busy"),
		false, false, false);
	reportSessionError(AccessSessionErrorDomain,
		RemoteBusySessionErrorCode, ApprovalSessionErrorStage,
		true, QStringLiteral("Remote peer is busy"));
	return KProtocolHandlerResult::success();
}

void KSessionCoordinator::handleInvalidSignalingMessage(KProtocolRouteStatus status,
	const QString &strError)
{
	const bool bWasAwaitingApproval = m_sessionStateMachine.isAwaitingApproval();
	++m_nInvalidSignalingMessages;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("protocol_reject"), QStringLiteral("signaling"), -1,
		QStringLiteral("status=%1 consecutive=%2 error=%3")
			.arg(static_cast<int>(status))
			.arg(m_nInvalidSignalingMessages)
			.arg(strError));
	if (m_sessionStateMachine.role() == ControlledSessionRole
		&& bWasAwaitingApproval)
	{
		rejectIncomingAccess(QStringLiteral("invalid_request"), false);
	}
	else if (bWasAwaitingApproval)
	{
		finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
	}
	else if (status == UnsupportedVersionProtocolRouteStatus
		|| m_nInvalidSignalingMessages >= KProtocolConstraints::kMaximumInvalidMessages)
	{
		finishSession(ConnectFailedSessionEndReason, QStringLiteral("protocol_violation"),
			m_sessionStateMachine.shouldKeepListening(), false, false);
	}
	else
	{
		return;
	}
	KSessionErrorCode code = ProtocolViolationSessionErrorCode;
	if (status == UnsupportedVersionProtocolRouteStatus)
		code = IncompatibleProtocolSessionErrorCode;
	else if (status == MalformedProtocolRouteStatus)
		code = MalformedMessageSessionErrorCode;
	reportSessionError(ProtocolSessionErrorDomain,
		code,
		bWasAwaitingApproval
			? ApprovalSessionErrorStage : NegotiationSessionErrorStage,
		false, strError.isEmpty()
			? QStringLiteral("Unsupported or invalid signaling message") : strError);
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
		publishSessionState();
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
		KSessionErrorCode code = ApprovalRejectedSessionErrorCode;
		bool bRetryable = false;
		if (strReason == QStringLiteral("busy"))
		{
			code = RemoteBusySessionErrorCode;
			bRetryable = true;
		}
		else if (strReason == QStringLiteral("timeout"))
		{
			code = ApprovalTimeoutSessionErrorCode;
			bRetryable = true;
		}
		else if (strReason == QStringLiteral("remote_access_disabled"))
		{
			code = RemoteAccessDisabledSessionErrorCode;
		}
		reportSessionError(AccessSessionErrorDomain, code,
			ApprovalSessionErrorStage, bRetryable,
			QStringLiteral("Access rejected: %1").arg(strReason));
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
	reportSessionError(AccessSessionErrorDomain,
		ApprovalTimeoutSessionErrorCode, ApprovalSessionErrorStage,
		true, QStringLiteral("Approval response timed out"));
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
	publishSessionState();
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
	publishSessionState();
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
	m_bSignalingConnected = false;
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
	if (!m_sessionStateMachine.beginReconnecting())
		return;

	m_pInputInjector->releaseAllInputs();
	publishSessionState();
	m_nReconnectGeneration = m_sessionStateMachine.generation();
	m_reconnectElapsedTimer.start();
	m_pReconnectTimer->start(kReconnectTimeoutMs);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_recovery_start"),
		QStringLiteral("ice_disconnected"),
		-1,
		QStringLiteral("generation=%1 signalingAvailable=%2 timeoutMs=%3")
			.arg(m_nReconnectGeneration)
			.arg(m_bSignalingConnected ? 1 : 0)
			.arg(kReconnectTimeoutMs));

	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	if (!m_bSignalingConnected)
	{
		KSessionTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("ice_restart_skipped"),
			QStringLiteral("signaling_unavailable"));
		return;
	}

	KSessionTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("ice_restart_requested"),
		QStringLiteral("request"),
		-1,
		QStringLiteral("attempt=1"));
	m_spRemotePeerTransport->restartIce();
}

void KSessionCoordinator::handlePeerConnectionRestored()
{
	if (!m_sessionStateMachine.restore())
		return;

	const qint64 nCostMs = m_reconnectElapsedTimer.isValid()
		? m_reconnectElapsedTimer.elapsed()
		: -1;
	m_pReconnectTimer->stop();
	m_nReconnectGeneration = 0;
	m_reconnectElapsedTimer.invalidate();
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_recovery_success"),
		QStringLiteral("restored"),
		-1,
		QStringLiteral("costMs=%1").arg(nCostMs));
	publishSessionState();
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

void KSessionCoordinator::handleReconnectTimeout()
{
	if (!m_sessionStateMachine.isReconnecting()
		|| m_nReconnectGeneration != m_sessionStateMachine.generation())
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
