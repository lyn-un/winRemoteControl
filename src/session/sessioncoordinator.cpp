#include "session/sessioncoordinator.h"

#include "session/accesssessionflow.h"
#include "session/capabilitysessionflow.h"
#include "session/mediasessioncontroller.h"
#include "session/peerlifecyclecontroller.h"
#include "session/recoverycontroller.h"
#include "session/sessioncommanddispatcher.h"
#include "session/securitysessionerrormapper.h"
#include "session/shutdowncoordinator.h"
#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "common/resourcetracelogger.h"
#include "core/input/inputinjectorinterface.h"
#include "core/protocol/accessmessage.h"
#include "core/protocol/protocolconstraints.h"
#include "core/session/capabilitynegotiator.h"
#include "core/protocol/webrtcsignalingmessage.h"
#include "core/session/deviceinfoprovider.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/trusteddevicestore.h"
#include "core/transport/signalingtransport.h"
#include "input/inputinjector.h"
#include "privacy/postsessionactionservice.h"
#include "privacy/privacymodeservice.h"

#include <QtCore/QTimer>
#include <utility>
#include <algorithm>

namespace
{
	constexpr int kReconnectTimeoutMs = 10000;

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

	static QString peerInitializationError(const KPeerInitializationResult &result)
	{
		return QStringLiteral("stage=%1 technical=%2")
			.arg(KPeerInitializationResult::stageName(result.stage),
				result.strTechnicalMessage);
	}

}

KSessionCoordinator::KSessionCoordinator(
	std::unique_ptr<IKDeviceInfoProvider> spDeviceInfoProvider,
	std::unique_ptr<IKInputInjector> spInputInjector,
	std::unique_ptr<KRemotePeerTransport> spRemotePeerTransport,
	std::unique_ptr<KSignalingTransport> spSignalingTransport,
	std::unique_ptr<KDeviceIdentityProvider> spIdentityProvider,
	std::unique_ptr<KTrustedDeviceStore> spTrustedDeviceStore,
	QObject *pParent)
	: KSessionController(pParent)
	, m_spDeviceInfoProvider(std::move(spDeviceInfoProvider))
	, m_spRemotePeerTransport(std::move(spRemotePeerTransport))
	, m_spSignalingTransport(std::move(spSignalingTransport))
	, m_spIdentityProvider(std::move(spIdentityProvider))
	, m_spTrustedDeviceStore(std::move(spTrustedDeviceStore))
	, m_pSignaling(m_spSignalingTransport.get())
	, m_pInputInjector(new KInputInjector(std::move(spInputInjector), this))
	, m_pAccessSessionFlow(new KAccessSessionFlow(m_pSignaling,
		m_spIdentityProvider.get(), m_spTrustedDeviceStore.get(), this))
	, m_pCapabilitySessionFlow(new KCapabilitySessionFlow(this))
	, m_pMediaSessionController(new KMediaSessionController(this))
	, m_pPeerLifecycleController(new KPeerLifecycleController(
		m_spRemotePeerTransport.get(), this))
	, m_pSessionCommandDispatcher(new KSessionCommandDispatcher(this))
	, m_pRecoveryController(new KRecoveryController(this))
	, m_pShutdownCoordinator(new KShutdownCoordinator(this))
{
	Q_ASSERT(m_spRemotePeerTransport != nullptr);
	Q_ASSERT(m_pSignaling != nullptr);
	Q_ASSERT(m_spIdentityProvider != nullptr);
	Q_ASSERT(m_spTrustedDeviceStore != nullptr);
	m_spTrustedDeviceStore->setIdentityProvider(m_spIdentityProvider.get());
	m_pAccessSessionFlow->setApplicationSettings(m_applicationSettings);
	initializeProtocolRoutes();
	initializeSessionHandlers();
	connect(m_pRecoveryController, &KRecoveryController::timedOut,
		this, &KSessionCoordinator::handleReconnectTimeout);
	connect(m_pShutdownCoordinator, &KShutdownCoordinator::finished,
		this, &KSessionCoordinator::finishStopping);
	connect(m_pShutdownCoordinator, &KShutdownCoordinator::watchdogExpired,
		this, &KSessionCoordinator::handleStopWatchdog);
	connect(m_pCapabilitySessionFlow, &KCapabilitySessionFlow::timedOut,
		this, &KSessionCoordinator::handleCapabilityTimeout);
	connect(m_pMediaSessionController, &KMediaSessionController::startCaptureRequested,
		this, &KSessionCoordinator::startCaptureRequested);
	connect(m_pMediaSessionController, &KMediaSessionController::stopCaptureRequested,
		this, &KSessionCoordinator::stopCaptureRequested);
	connect(m_pPeerLifecycleController, &KPeerLifecycleController::rollbackTimeout,
		this, &KSessionCoordinator::handlePeerInitializationRollbackTimeout);
	connect(m_pPeerLifecycleController, &KPeerLifecycleController::rollbackFinished,
		this, &KSessionCoordinator::handlePeerInitializationRollbackFinished);
	connect(m_pPeerLifecycleController, &KPeerLifecycleController::peerShutdownFinished,
		this, &KSessionCoordinator::handlePeerShutdownFinished);
	m_pSessionCommandDispatcher->setTransmitFunction(
		[this](const KSessionMessage &message) { return transmitSessionMessage(message); });
	connect(m_pSessionCommandDispatcher, &KSessionCommandDispatcher::commandCompleted,
		this, &KSessionCoordinator::handleSessionCommandCompleted);
	connect(m_pSessionCommandDispatcher, &KSessionCommandDispatcher::commandTimedOut,
		this, &KSessionCoordinator::handleSessionCommandTimeout);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::stateChanged,
		this, &KSessionCoordinator::signalingChanged);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::signalingError,
		this, [this](const QString &strMessage)
		{
			const KSessionState state = m_sessionStateMachine.state();
			if (m_sessionStateMachine.isStopping()
				|| state == ShutdownTimedOutSessionState
				|| state == IdleSessionState
				|| state == ListeningSessionState)
			{
				KSessionTraceLogger::write(
					roleToString(m_sessionStateMachine.role()),
					QStringLiteral("signaling_closed_after_session_end"),
					QStringLiteral("expected"), -1,
					QStringLiteral("generation=%1 state=%2 technical=%3")
						.arg(m_sessionStateMachine.generation())
						.arg(KSessionStateMachine::stateName(state), strMessage));
				return;
			}
			const KSessionErrorStage stage = m_sessionStateMachine.isNegotiating()
				? NegotiationSessionErrorStage : ConnectedSessionErrorStage;
			reportSessionError(SignalingSessionErrorDomain,
				ConnectionFailedSessionErrorCode, stage,
				true, strMessage);
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::outgoingConnectionEstablished,
		this, &KSessionCoordinator::handleOutgoingConnectionEstablished);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::outgoingConnectionFailed,
		this, &KSessionCoordinator::handleOutgoingConnectionFailed);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingConnectionEstablished,
		this, &KSessionCoordinator::handleIncomingConnectionEstablished);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::connectionLost,
		this, &KSessionCoordinator::handleSignalingConnectionLost);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingAccessObserved,
		this, &KSessionCoordinator::incomingAccessObserved);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingAccessRequest,
		this, &KSessionCoordinator::incomingAccessRequest);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingAccessRequestCleared,
		this, &KSessionCoordinator::incomingAccessRequestCleared);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::pairingRequested,
		this, [this](const QString &strRequestId,
			const QString &strDeviceName,
			const QString &strLocalRole,
			const QString &strVerificationCode,
			const QString &strControllerFingerprint,
			const QString &strControlledFingerprint,
			const QString &strTlsProtocol,
			const QString &strCipherSuite,
			KPermissionScopes permissions,
			qint64 nExpiresAtMs)
		{
			if (m_sessionStateMachine.isAuthenticatingIdentity())
			{
				m_sessionStateMachine.beginPairing();
				publishSessionState();
			}
			KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
				QStringLiteral("device_pairing"), QStringLiteral("requested"), -1,
				QStringLiteral("method=tls_exporter_numeric_v1 generation=%1 permissions=%2")
					.arg(sessionGeneration()).arg(permissions.toInt()));
			emit pairingRequested(strRequestId, strDeviceName,
				strLocalRole, strVerificationCode,
				strControllerFingerprint, strControlledFingerprint,
				strTlsProtocol, strCipherSuite,
				permissions, nExpiresAtMs);
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::pairingCleared,
		this, &KSessionCoordinator::pairingCleared);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::identityAuthenticated,
		this, [this](const KDeviceAuthenticationContext &context)
		{
			if (!m_sessionStateMachine.completeIdentityAuthentication())
				return;
			m_effectivePermissions = context.effectivePermissions;
			KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
				QStringLiteral("device_authentication"),
				QStringLiteral("authenticated"), -1,
				QStringLiteral("deviceId=%1 fingerprint=%2 permissions=%3 trusted=%4")
					.arg(context.strRemoteDeviceId,
						context.strRemoteFingerprint.left(12))
					.arg(context.effectivePermissions.toInt())
					.arg(context.bTrustedDevice ? 1 : 0));
			emit deviceAuthenticationStateChanged(QStringLiteral("authenticated"),
				context.strRemoteDeviceId, context.strRemoteFingerprint,
				context.bTrustedDevice);
			publishSessionState();
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingAccessAccepted,
		this, [this]()
		{
			if (m_sessionStateMachine.role() != ControlledSessionRole
				|| !m_sessionStateMachine.isAwaitingApproval()
				|| !m_sessionStateMachine.approveConnection())
				return;
			publishSessionState();
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingAccessRejected,
		this, [this](const QString &)
		{
			if (m_sessionStateMachine.role() != ControlledSessionRole
				|| (!m_sessionStateMachine.isAwaitingApproval()
					&& !m_sessionStateMachine.isAuthenticatingIdentity()
					&& !m_sessionStateMachine.isPairing()))
				return;
			m_sessionStateMachine.rejectConnection();
			m_pAccessSessionFlow->disconnectPeer();
			updateListeningAvailability(true, m_nListeningPort);
			publishSessionState();
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::incomingSecurityRejected,
		this, &KSessionCoordinator::handleIncomingSecurityRejected);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::outgoingAccessAccepted,
		this, [this]()
		{
			if (m_sessionStateMachine.role() != ControllerSessionRole
				|| !m_sessionStateMachine.isAwaitingApproval()
				|| !m_sessionStateMachine.approveConnection())
				return;
			publishSessionState();
			m_spRemotePeerTransport->createOffer();
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::outgoingAccessRejected,
		this, &KSessionCoordinator::handleOutgoingAccessRejected);
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::outgoingSecurityRejected,
		this, &KSessionCoordinator::handleOutgoingSecurityRejected);
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

KSessionRole KSessionCoordinator::sessionRole() const
{
	return m_sessionStateMachine.role();
}

bool KSessionCoordinator::isIdle() const
{
	return m_sessionStateMachine.state() == IdleSessionState;
}

bool KSessionCoordinator::matchesCurrentEndpoint(
	const QString &strHost,
	quint16 nPort) const
{
	return m_sessionStateMachine.role() == ControllerSessionRole
		&& m_sessionStateMachine.state() != IdleSessionState
		&& m_pAccessSessionFlow->matchesEndpoint(strHost, nPort);
}

KNegotiatedCapabilities KSessionCoordinator::negotiatedCapabilities() const
{
	return m_pCapabilitySessionFlow->negotiatedCapabilities();
}

QString KSessionCoordinator::authenticatedDeviceId() const
{
	return m_pAccessSessionFlow->authenticationContext().strRemoteDeviceId;
}

void KSessionCoordinator::setTerminalCapabilitiesAvailable(
	bool bControllerAvailable,
	bool bControlledAvailable)
{
	m_bControllerTerminalCapabilityAvailable = bControllerAvailable;
	m_bControlledTerminalCapabilityAvailable = bControlledAvailable;
}

void KSessionCoordinator::configurePrivacyServices(
	std::unique_ptr<KPrivacyModeService> spPrivacyModeService,
	std::unique_ptr<KPostSessionActionService> spPostSessionActionService)
{
	Q_ASSERT(spPrivacyModeService != nullptr);
	Q_ASSERT(spPostSessionActionService != nullptr);
	Q_ASSERT(m_spPrivacyModeService == nullptr);
	Q_ASSERT(m_spPostSessionActionService == nullptr);
	m_spPrivacyModeService = std::move(spPrivacyModeService);
	m_spPostSessionActionService = std::move(spPostSessionActionService);
	connect(m_spPrivacyModeService.get(), &KPrivacyModeService::statusChanged,
		this, &KSessionCoordinator::publishPrivacyModeStatus);
	connect(m_spPostSessionActionService.get(), &KPostSessionActionService::statusChanged,
		this, &KSessionCoordinator::publishPostSessionActionStatus);
}

void KSessionCoordinator::initializeProtocolRoutes()
{
	const KProtocolRouter::Guard identityHandshake =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
		{
			return context.nState == static_cast<int>(AuthenticatingIdentitySessionState)
				|| context.nState == static_cast<int>(PairingSessionState);
		};
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
	const KProtocolRouter::Handler accessHandler =
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return handleAccessEnvelope(envelope); };
	const KProtocolRouter::Handler pairingHandler =
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{
			KTlsPairingMessage message;
			QString strError;
			if (!KTlsPairingMessageCodec::decode(envelope, &message, &strError))
				return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
			return m_pAccessSessionFlow->handleTlsPairingMessage(message,
				m_sessionStateMachine.generation())
				? KProtocolHandlerResult::success()
				: KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
					QStringLiteral("TLS pairing message does not match the active handshake"));
		};
	for (KTlsPairingMessageType type : { HelloTlsPairingMessageType,
		DecisionTlsPairingMessageType, ReadyTlsPairingMessageType,
		CommittedTlsPairingMessageType,
		RejectedTlsPairingMessageType })
	{
		m_protocolRouter.registerHandler(SignalingProtocolChannel,
			KTlsPairingMessageCodec::typeName(type), identityHandshake, pairingHandler);
	}
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
	m_protocolRouter.registerHandler(SignalingProtocolChannel,
		KAccessMessageCodec::typeName(ServerBusyAccessMessageType),
		controllerAwaitingApproval,
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return handleBusyEnvelope(envelope); });
	const KProtocolRouter::Handler signalingHandler =
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return handleWebRtcSignalingEnvelope(envelope); };
	const KProtocolRouter::Guard secureSignalingAllowed =
		[acceptsWebRtcSignalingState](const KProtocolEnvelope &,
			const KProtocolRouteContext &context)
		{
			return acceptsWebRtcSignalingState(context);
		};
	for (KWebRtcSignalingMessageType type : { OfferWebRtcSignalingMessageType,
		AnswerWebRtcSignalingMessageType, IceCandidateWebRtcSignalingMessageType })
	{
		m_protocolRouter.registerHandler(SignalingProtocolChannel,
			KWebRtcSignalingMessageCodec::typeName(type),
			secureSignalingAllowed, signalingHandler);
	}
}

void KSessionCoordinator::initializeSessionHandlers()
{
	m_pSessionCommandDispatcher->registerHandler(DeviceInfoRequestSessionMessageType,
		[this](const KSessionMessage &message) { return handleDeviceInfoRequestMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(DeviceInfoSessionMessageType,
		[this](const KSessionMessage &message) { return handleDeviceInfoMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(StartStreamingSessionMessageType,
		[this](const KSessionMessage &message) { return handleStartStreamingMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(StopStreamingSessionMessageType,
		[this](const KSessionMessage &message) { return handleStopStreamingMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(EndSessionMessageType,
		[this](const KSessionMessage &message) { return handleEndSessionMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(StreamConfigSessionMessageType,
		[this](const KSessionMessage &message) { return handleStreamConfigMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(CapabilitiesSessionMessageType,
		[this](const KSessionMessage &message) { return handleCapabilitiesMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(CapabilityRejectedSessionMessageType,
		[this](const KSessionMessage &message) { return handleCapabilityRejectedMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(SetPrivacyModeSessionMessageType,
		[this](const KSessionMessage &message) { return handleSetPrivacyModeMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(PrivacyModeStateSessionMessageType,
		[this](const KSessionMessage &message) { return handlePrivacyModeStateMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(SetPostSessionActionSessionMessageType,
		[this](const KSessionMessage &message) { return handleSetPostSessionActionMessage(message); });
	m_pSessionCommandDispatcher->registerHandler(PostSessionActionStateSessionMessageType,
		[this](const KSessionMessage &message) { return handlePostSessionActionStateMessage(message); });
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
	if (m_sessionStateMachine.isShutdownTimedOut())
	{
		reportSessionError(ShutdownSessionErrorDomain,
			ShutdownTimeoutSessionErrorCode, ShutdownSessionErrorStage,
			false, QStringLiteral("Cannot change role while shutdown is incomplete"));
		return;
	}
	KSessionRole role;
	if (!KSessionStateMachine::roleFromString(strRole, &role))
		return;
	if (m_pPeerLifecycleController->rollbackPending())
	{
		m_pendingRequestType = RolePendingRequest;
		m_pendingRole = role;
		return;
	}
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
		m_pAccessSessionFlow->clearLastEndpoint();
	}

	emit webRtcStateChanged(QStringLiteral("Role:%1")
		.arg(KSessionStateMachine::roleName(m_sessionStateMachine.role())));
}

void KSessionCoordinator::startSignalingServer(quint16 nPort)
{
	if (m_sessionStateMachine.isShutdownTimedOut())
	{
		reportSessionError(ShutdownSessionErrorDomain,
			ShutdownTimeoutSessionErrorCode, ShutdownSessionErrorStage,
			false, QStringLiteral("Cannot listen while shutdown is incomplete"));
		return;
	}
	m_pAccessSessionFlow->setConnected(false);
	if (!m_applicationSettings.bRemoteAccessEnabled)
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			RemoteAccessDisabledSessionErrorCode, ListeningSessionErrorStage,
			false, QStringLiteral("Remote access is disabled"));
		return;
	}
	if (m_pPeerLifecycleController->rollbackPending())
	{
		m_pendingRequestType = ListenPendingRequest;
		m_nPendingPort = nPort;
		return;
	}
	if (m_sessionStateMachine.state() != IdleSessionState)
	{
		m_pendingRequestType = ListenPendingRequest;
		m_nPendingPort = nPort;
		finishSession(RestartListenerSessionEndReason, QString(), false, true, false);
		return;
	}
	const KPeerInitializationResult initializationResult =
		initializePeer(ControlledSessionRole);
	if (!initializationResult.succeeded())
	{
		reportSessionError(WebRtcSessionErrorDomain,
			InitializationFailedSessionErrorCode, StartupSessionErrorStage,
			false, peerInitializationError(initializationResult));
		return;
	}

	QString strError;
	if (!m_pAccessSessionFlow->startListening(nPort, &strError))
	{
		updateListeningAvailability(false);
		m_spRemotePeerTransport->requestShutdown(m_nActivePeerGeneration);
		reportSessionError(SignalingSessionErrorDomain,
			ConnectionFailedSessionErrorCode, ListeningSessionErrorStage,
			true, strError);
		return;
	}

	m_sessionStateMachine.beginListening();
	updateListeningAvailability(true, m_pAccessSessionFlow->listeningPort());
	publishSessionState();
}

void KSessionCoordinator::connectSignaling(const QString &strHost, quint16 nPort)
{
	if (m_sessionStateMachine.isShutdownTimedOut())
	{
		reportSessionError(ShutdownSessionErrorDomain,
			ShutdownTimeoutSessionErrorCode, ShutdownSessionErrorStage,
			false, QStringLiteral("Cannot connect while shutdown is incomplete"));
		return;
	}
	const QString strTargetHost = strHost;
	if (strTargetHost.isEmpty() || nPort == 0)
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			InvalidArgumentSessionErrorCode, ConnectingSessionErrorStage,
			false, QStringLiteral("Invalid controlled endpoint"));
		return;
	}
	if (m_pPeerLifecycleController->rollbackPending())
	{
		m_pendingRequestType = ConnectPendingRequest;
		m_strPendingHost = strTargetHost;
		m_nPendingPort = nPort;
		return;
	}
	m_pAccessSessionFlow->setConnected(false);
	if (m_sessionStateMachine.state() != IdleSessionState)
	{
		m_pendingRequestType = ConnectPendingRequest;
		m_strPendingHost = strTargetHost;
		m_nPendingPort = nPort;
		finishSession(NewConnectionSessionEndReason, QString(), false, true, false);
		return;
	}
	m_pAccessSessionFlow->stop();
	const KPeerInitializationResult initializationResult =
		initializePeer(ControllerSessionRole);
	if (!initializationResult.succeeded())
	{
		reportSessionError(WebRtcSessionErrorDomain,
			InitializationFailedSessionErrorCode, StartupSessionErrorStage,
			false, peerInitializationError(initializationResult));
		return;
	}

	m_sessionStateMachine.beginConnecting();
	publishSessionState();
	m_pAccessSessionFlow->connectToHost(strTargetHost, nPort);
}

void KSessionCoordinator::retryLastConnection()
{
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| m_sessionStateMachine.state() != IdleSessionState
		|| !m_pAccessSessionFlow->hasLastEndpoint())
	{
		reportSessionError(ConfigurationSessionErrorDomain,
			InvalidArgumentSessionErrorCode, ConnectingSessionErrorStage,
			false, QStringLiteral("No last connection endpoint"));
		return;
	}

	const QString strHost = m_pAccessSessionFlow->lastHost();
	const quint16 nPort = m_pAccessSessionFlow->lastPort();
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
	if (m_pPeerLifecycleController->rollbackPending())
		return;
	finishSession(LocalDisconnectSessionEndReason, QString(), false, true, false);
}

void KSessionCoordinator::applyApplicationSettings(const KApplicationSettings &settings)
{
	const bool bWasEnabled = m_applicationSettings.bRemoteAccessEnabled;
	m_applicationSettings = SanitizeApplicationSettings(settings);
	m_pAccessSessionFlow->setApplicationSettings(m_applicationSettings);
	if (bWasEnabled && !m_applicationSettings.bRemoteAccessEnabled
		&& m_sessionStateMachine.role() == ControlledSessionRole
		&& m_sessionStateMachine.state() != IdleSessionState)
	{
		const bool bPendingApproval = (m_sessionStateMachine.isAwaitingApproval()
			|| m_sessionStateMachine.isAuthenticatingIdentity()
			|| m_sessionStateMachine.isPairing())
			&& m_pAccessSessionFlow->hasApproval();
		if (bPendingApproval)
			m_pAccessSessionFlow->cancelApproval(
				QStringLiteral("remote_access_disabled"), true);
		finishSession(LocalDisconnectSessionEndReason,
			QStringLiteral("remote_access_disabled"), false, !bPendingApproval, false);
	}
}

void KSessionCoordinator::respondIncomingAccessRequest(
	const QString &strRequestId,
	bool bAccepted)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| !m_sessionStateMachine.isAwaitingApproval())
	{
		return;
	}
	m_pAccessSessionFlow->respondIncoming(strRequestId, bAccepted);
}

void KSessionCoordinator::respondPairingRequest(const QString &strRequestId,
	bool bAccepted,
	KPermissionScopes permissions)
{
	if (!m_sessionStateMachine.isPairing())
		return;
	m_pAccessSessionFlow->respondPairing(strRequestId, bAccepted, permissions);
}

void KSessionCoordinator::requestTrustedDevices()
{
	QString strError;
	const QVector<KTrustedDevice> devices = m_spTrustedDeviceStore->loadDevices(
		&strError);
	if (!strError.isEmpty())
	{
		emit trustedDeviceError(strError);
		return;
	}
	const QString strMigrationNotice = m_spTrustedDeviceStore->takeMigrationNotice();
	if (!strMigrationNotice.isEmpty())
		emit securityMigrationNotice(strMigrationNotice);
	emit trustedDevicesChanged(devices);
}

void KSessionCoordinator::updateTrustedDevice(const QString &strDeviceId,
	const QString &strAlias,
	KPermissionScopes permissions)
{
	QString strError;
	QVector<KTrustedDevice> devices = m_spTrustedDeviceStore->loadDevices(&strError);
	if (!strError.isEmpty())
	{
		emit trustedDeviceError(strError);
		return;
	}
	bool bFound = false;
	for (KTrustedDevice &device : devices)
	{
		if (device.strDeviceId != strDeviceId)
			continue;
		device.strAlias = strAlias.trimmed().left(128);
		device.permissionLimit = permissions | ViewScreenPermissionScope;
		bFound = true;
		break;
	}
	if (!bFound)
	{
		emit trustedDeviceError(QStringLiteral("Trusted device does not exist"));
		return;
	}
	if (!m_spTrustedDeviceStore->saveDevices(devices, &strError))
	{
		emit trustedDeviceError(strError);
		return;
	}
	requestTrustedDevices();
}

void KSessionCoordinator::revokeTrustedDevice(const QString &strDeviceId)
{
	QString strError;
	QVector<KTrustedDevice> devices = m_spTrustedDeviceStore->loadDevices(&strError);
	bool bFound = false;
	for (KTrustedDevice &device : devices)
	{
		if (device.strDeviceId == strDeviceId)
		{
			device.bRevoked = true;
			bFound = true;
		}
	}
	if (!strError.isEmpty())
	{
		emit trustedDeviceError(strError);
		return;
	}
	if (!bFound)
	{
		emit trustedDeviceError(QStringLiteral("Trusted device does not exist"));
		return;
	}
	if (!m_spTrustedDeviceStore->saveDevices(devices, &strError))
	{
		emit trustedDeviceError(strError.isEmpty()
			? QStringLiteral("Failed to revoke trusted device") : strError);
		return;
	}
	emit trustedDeviceRevoked(strDeviceId);
	requestTrustedDevices();
}

void KSessionCoordinator::requestRePairDevice(const QString &strDeviceId)
{
	QString strError;
	QVector<KTrustedDevice> devices = m_spTrustedDeviceStore->loadDevices(&strError);
	bool bFound = false;
	for (qsizetype nIndex = devices.size(); nIndex-- > 0;)
	{
		if (devices.at(nIndex).strDeviceId == strDeviceId)
		{
			devices.removeAt(nIndex);
			bFound = true;
		}
	}
	if (!strError.isEmpty())
	{
		emit trustedDeviceError(strError);
		return;
	}
	if (!bFound)
	{
		emit trustedDeviceError(QStringLiteral("Trusted device does not exist"));
		return;
	}
	if (!m_spTrustedDeviceStore->saveDevices(devices, &strError))
	{
		emit trustedDeviceError(strError.isEmpty()
			? QStringLiteral("Failed to remove trusted device") : strError);
		return;
	}
	emit trustedDeviceRevoked(strDeviceId);
	requestTrustedDevices();
}

void KSessionCoordinator::enterRemoteDesktop(const KStreamConfig &config)
{
	if (!hasPermission(ViewScreenPermissionScope)
		|| !m_sessionStateMachine.canEnterRemoteDesktop() || !m_bSessionChannelOpen)
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
	message.streamConfig = m_pMediaSessionController->constrainedConfig(config);
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
	m_pMediaSessionController->stopCapture(m_sessionStateMachine.generation());
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
	if (!hasPermission(ViewScreenPermissionScope)
		|| !m_bSessionChannelOpen
		|| !m_sessionStateMachine.canStartControlledStreaming())
	{
		if (!hasPermission(ViewScreenPermissionScope))
			publishPermissionDenied(QStringLiteral("video_start"));
		return;
	}

	m_pMediaSessionController->startCapture(m_sessionStateMachine.generation());
	m_sessionStateMachine.beginStreaming();
	if (m_spPostSessionActionService != nullptr)
		m_spPostSessionActionService->markStreaming(m_sessionStateMachine.generation());
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
	if (hasPermission(ViewScreenPermissionScope)
		&& m_sessionStateMachine.canSendVideo())
		m_spRemotePeerTransport->pushVideoFrame(frame);
}

void KSessionCoordinator::sendInputMessage(const KInputMessage &message)
{
	if (!hasPermission(InputControlPermissionScope)
		|| !m_sessionStateMachine.canSendInput() || !m_bInputChannelOpen)
	{
		if (!hasPermission(InputControlPermissionScope))
			publishPermissionDenied(QStringLiteral("input_send"));
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
	if (!hasPermission(ClipboardPermissionScope)
		|| !m_sessionStateMachine.canSyncClipboard() || !m_bClipboardChannelOpen)
	{
		if (!hasPermission(ClipboardPermissionScope))
			publishPermissionDenied(QStringLiteral("clipboard_send"));
		return;
	}
	m_spRemotePeerTransport->sendClipboardMessage(message);
}

bool KSessionCoordinator::sendTerminalControlMessage(const KTerminalMessage &message)
{
	if (!hasPermission(TerminalPermissionScope)
		|| !m_pCapabilitySessionFlow->isComplete()
		|| !m_pCapabilitySessionFlow->negotiatedCapabilities().channels.contains(
			QStringLiteral("terminal")))
	{
		if (!hasPermission(TerminalPermissionScope))
			publishPermissionDenied(QStringLiteral("terminal_control_send"));
		return false;
	}
	return m_spRemotePeerTransport->sendTerminalControlMessage(message)
		== KRemotePeerTransport::SessionMessageAccepted;
}

bool KSessionCoordinator::sendTerminalData(const QByteArray &data)
{
	if (!hasPermission(TerminalPermissionScope)
		|| !m_bTerminalChannelOpen
		|| !m_pCapabilitySessionFlow->negotiatedCapabilities().channels.contains(
			QStringLiteral("terminal")))
	{
		if (!hasPermission(TerminalPermissionScope))
			publishPermissionDenied(QStringLiteral("terminal_data_send"));
		return false;
	}
	return m_spRemotePeerTransport->sendTerminalData(data);
}

bool KSessionCoordinator::isTerminalBackpressured() const
{
	return m_spRemotePeerTransport->terminalBackpressured();
}

bool KSessionCoordinator::ensureFileTransferChannels()
{
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| !hasPermission(FileTransferPermissionScope)
		|| !m_pCapabilitySessionFlow->isComplete())
	{
		if (!hasPermission(FileTransferPermissionScope))
			publishPermissionDenied(QStringLiteral("file_transfer_open"));
		return false;
	}
	const QStringList &channels =
		m_pCapabilitySessionFlow->negotiatedCapabilities().channels;
	if (!channels.contains(QStringLiteral("file-control"))
		|| !channels.contains(QStringLiteral("file-data")))
	{
		return false;
	}
	return m_spRemotePeerTransport->ensureFileTransferChannels();
}

bool KSessionCoordinator::sendFileTransferLifecycleMessage(
	const KFileTransferLifecycleMessage &message)
{
	if (!hasPermission(FileTransferPermissionScope)
		|| !m_pCapabilitySessionFlow->isComplete())
	{
		if (!hasPermission(FileTransferPermissionScope))
			publishPermissionDenied(QStringLiteral("file_transfer_lifecycle_send"));
		return false;
	}
	return m_spRemotePeerTransport->sendFileTransferLifecycleMessage(message)
		== KRemotePeerTransport::SessionMessageAccepted;
}

bool KSessionCoordinator::sendFileTransferControlMessage(
	const KFileTransferControlMessage &message)
{
	if (!hasPermission(FileTransferPermissionScope)
		|| !m_bFileTransferControlChannelOpen
		|| !m_pCapabilitySessionFlow->isComplete())
	{
		if (!hasPermission(FileTransferPermissionScope))
			publishPermissionDenied(QStringLiteral("file_transfer_control_send"));
		return false;
	}
	return m_spRemotePeerTransport->sendFileTransferControlMessage(message);
}

bool KSessionCoordinator::sendFileTransferData(const QByteArray &data)
{
	if (!hasPermission(FileTransferPermissionScope)
		|| !m_bFileTransferDataChannelOpen
		|| !m_pCapabilitySessionFlow->isComplete())
	{
		if (!hasPermission(FileTransferPermissionScope))
			publishPermissionDenied(QStringLiteral("file_transfer_data_send"));
		return false;
	}
	return m_spRemotePeerTransport->sendFileTransferData(data);
}

bool KSessionCoordinator::isFileTransferBackpressured() const
{
	return m_spRemotePeerTransport->fileTransferBackpressured();
}

QString KSessionCoordinator::sendSessionMessage(KSessionMessage message)
{
	return m_pSessionCommandDispatcher->send(std::move(message),
		m_sessionStateMachine.generation());
}

KSessionCommandTransmitResult KSessionCoordinator::transmitSessionMessage(
	const KSessionMessage &message)
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
		return { false, QStringLiteral("session_channel_closed") };
	}

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("send"),
		strType,
		strMessage.toUtf8().size());
	const KRemotePeerTransport::KSessionMessageSendStatus status =
		m_spRemotePeerTransport->sendSessionMessage(message);
	if (status == KRemotePeerTransport::SessionMessageAccepted)
		return { true, QString() };
	QString strErrorCode = QStringLiteral("send_failed");
	if (status == KRemotePeerTransport::SessionMessageChannelUnavailable)
		strErrorCode = QStringLiteral("session_channel_closed");
	else if (status == KRemotePeerTransport::SessionMessageQueueOverflow)
		strErrorCode = QStringLiteral("command_queue_overflow");

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("send_failed"), strType, strMessage.toUtf8().size(),
		QStringLiteral("requestId=%1").arg(message.strRequestId));
	if (!KSessionMessageCodec::isCommand(message.type))
	{
		const KSessionErrorCode errorCode = status
			== KRemotePeerTransport::SessionMessageQueueOverflow
			? CommandQueueOverflowSessionErrorCode : SendFailedSessionErrorCode;
		reportSessionError(ProtocolSessionErrorDomain,
			errorCode,
			ConnectedSessionErrorStage,
			false,
			QStringLiteral("Session message send failed: %1").arg(strErrorCode));
	}
	return { false, strErrorCode };
}

void KSessionCoordinator::handleSessionCommandCompleted(KSessionMessageType type,
	const QString &strRequestId,
	bool bSuccess,
	const QString &strErrorCode,
	quint64 nGeneration)
{
	if (nGeneration != m_sessionStateMachine.generation())
		return;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_command_result"),
		KSessionMessageCodec::typeName(type), -1,
		QStringLiteral("requestId=%1 success=%2 errorCode=%3")
			.arg(strRequestId)
			.arg(bSuccess ? 1 : 0)
			.arg(strErrorCode));
	if (type == EndSessionMessageType && m_sessionStateMachine.isStopping())
	{
		m_strPendingEndCommandId.clear();
		continueStoppingTeardown();
		return;
	}
	if (type == SetPrivacyModeSessionMessageType)
	{
		emit privacyModeCommandCompleted(strRequestId, bSuccess, strErrorCode);
		return;
	}
	if (type == SetPostSessionActionSessionMessageType)
	{
		emit postSessionActionCommandCompleted(strRequestId, bSuccess, strErrorCode);
		return;
	}
	if (!bSuccess && !m_sessionStateMachine.isStopping())
	{
		KSessionErrorCode errorCode = InvalidStateSessionErrorCode;
		if (strErrorCode == QStringLiteral("send_failed")
			|| strErrorCode == QStringLiteral("session_channel_closed"))
		{
			errorCode = SendFailedSessionErrorCode;
		}
		else if (strErrorCode == QStringLiteral("command_queue_overflow"))
		{
			errorCode = CommandQueueOverflowSessionErrorCode;
		}
		reportSessionError(ProtocolSessionErrorDomain,
			errorCode, ConnectedSessionErrorStage,
			false, QStringLiteral("Remote rejected session command: %1")
				.arg(strErrorCode));
	}
}

void KSessionCoordinator::handleSessionCommandTimeout(KSessionMessageType type,
	const QString &strRequestId,
	quint64 nGeneration)
{
	if (nGeneration != m_sessionStateMachine.generation())
		return;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_command_timeout"),
		KSessionMessageCodec::typeName(type), -1,
		QStringLiteral("requestId=%1 generation=%2")
			.arg(strRequestId)
			.arg(nGeneration));
	if (type == EndSessionMessageType && m_sessionStateMachine.isStopping())
	{
		m_strPendingEndCommandId.clear();
		continueStoppingTeardown();
		return;
	}
	if (type == SetPrivacyModeSessionMessageType)
	{
		emit privacyModeCommandCompleted(strRequestId, false, QStringLiteral("command_timeout"));
		return;
	}
	if (type == SetPostSessionActionSessionMessageType)
	{
		emit postSessionActionCommandCompleted(strRequestId, false,
			QStringLiteral("command_timeout"));
		return;
	}
	reportSessionError(ProtocolSessionErrorDomain,
		CommandTimeoutSessionErrorCode, ConnectedSessionErrorStage,
		false, QStringLiteral("Session command timed out: %1")
			.arg(KSessionMessageCodec::typeName(type)));
	finishSession(DisconnectTimeoutSessionEndReason,
		QStringLiteral("session_command_timeout"),
		m_sessionStateMachine.shouldKeepListening(), false, false);
}

void KSessionCoordinator::sendStreamConfig(const KStreamConfig &config)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	KSessionMessage message;
	message.type = StreamConfigSessionMessageType;
	message.streamConfig = m_pMediaSessionController->constrainedConfig(config);
	sendSessionMessage(message);
}

QString KSessionCoordinator::requestPrivacyMode(KPrivacyMode mode)
{
	const KSessionState state = m_sessionStateMachine.state();
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| (state != StreamingSessionState && state != ReconnectingSessionState)
		|| !m_pCapabilitySessionFlow->isComplete())
	{
		emit privacyModeCommandCompleted(QString(), false,
			QStringLiteral("invalid_session_state"));
		return QString();
	}
	if (!hasPermission(ViewScreenPermissionScope)
		|| !hasPermission(InputControlPermissionScope))
	{
		emit privacyModeCommandCompleted(QString(), false,
			QStringLiteral("permission_denied"));
		return QString();
	}
	QString strMode;
	if (mode == DisabledPrivacyMode)
		strMode = QStringLiteral("disabled");
	else if (mode == PrivacyOverlayPrivacyMode)
		strMode = QStringLiteral("privacyoverlay");
	else if (mode == DisplayOffPrivacyMode)
		strMode = QStringLiteral("displayoff");
	if (strMode.isEmpty()
		|| !m_pCapabilitySessionFlow->negotiatedCapabilities()
			.supportedPrivacyModes.contains(strMode))
	{
		emit privacyModeCommandCompleted(QString(), false,
			QStringLiteral("unsupported_mode"));
		return QString();
	}
	KSessionMessage message;
	message.type = SetPrivacyModeSessionMessageType;
	message.privacyMode = mode;
	return sendSessionMessage(message);
}

QString KSessionCoordinator::requestPostSessionAction(KPostSessionAction action)
{
	const KSessionState state = m_sessionStateMachine.state();
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| (state != StreamingSessionState && state != ReconnectingSessionState)
		|| !m_pCapabilitySessionFlow->isComplete())
	{
		emit postSessionActionCommandCompleted(QString(), false,
			QStringLiteral("invalid_session_state"));
		return QString();
	}
	if (action == UnknownPostSessionAction
		|| (action == LockWorkstationPostSessionAction
			&& !m_pCapabilitySessionFlow->negotiatedCapabilities().bPostSessionLock))
	{
		emit postSessionActionCommandCompleted(QString(), false,
			QStringLiteral("unsupported_action"));
		return QString();
	}
	KSessionMessage message;
	message.type = SetPostSessionActionSessionMessageType;
	message.postSessionAction = action;
	return sendSessionMessage(message);
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
	const bool bCancelApproval = m_sessionStateMachine.isAwaitingApproval()
		|| m_sessionStateMachine.isAuthenticatingIdentity()
		|| m_sessionStateMachine.isPairing();
	if (!m_sessionStateMachine.beginStopping())
		return;
	// Enter Stopping before sending any cancellation message. The peer may close
	// the TLS socket synchronously in response; that close is expected teardown,
	// not a new signaling failure for the session being rejected.
	if (bCancelApproval)
		m_pAccessSessionFlow->cancelApproval(QStringLiteral("cancelled"), bNotifyRemote);
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
	if (role == ControlledSessionRole && m_spPrivacyModeService != nullptr)
		m_spPrivacyModeService->reset(nGeneration);
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
				.arg(m_pRecoveryController->elapsedMs()));
	}
	m_pRecoveryController->clear();
	m_pCapabilitySessionFlow->reset();
	m_pAccessSessionFlow->setConnected(false);
	m_pAccessSessionFlow->clearApproval(strReason);
	m_bDeviceInfoRequested = false;
	m_nInvalidSignalingMessages = 0;
	m_bInputChannelOpen = false;
	m_bClipboardChannelOpen = false;
	m_bTerminalChannelOpen = false;
	m_bTerminalChannelStatePublished = false;
	m_bFileTransferControlChannelOpen = false;
	m_bFileTransferDataChannelOpen = false;
	m_bFileTransferChannelStatePublished = false;
	m_bSessionChannelOpen = false;
	emit sessionCapabilitiesChanged(KNegotiatedCapabilities());
	m_pSessionCommandDispatcher->failAll(QStringLiteral("session_stopping"));
	m_pShutdownCoordinator->begin(nGeneration,
		m_pMediaSessionController->isCaptureActive(), true, 3000);
	m_pMediaSessionController->stopCapture(nGeneration);
	m_pMediaSessionController->reset();
	emit sessionChannelChanged(false);
	emit fileTransferChannelsChanged(false, false);
	emit networkStatsReady(KNetworkStats());
	m_pPeerLifecycleController->requestShutdown(nGeneration);
}

void KSessionCoordinator::handleCaptureShutdownFinished(quint64 nGeneration)
{
	if (nGeneration != m_nStoppingGeneration
		|| !m_sessionStateMachine.canCompleteShutdown())
		return;
	m_pShutdownCoordinator->completeCapture(nGeneration);
}

void KSessionCoordinator::handlePeerShutdownFinished(quint64 nGeneration)
{
	if (nGeneration != m_nStoppingGeneration
		|| !m_sessionStateMachine.canCompleteShutdown())
		return;
	KResourceTraceLogger::write(
		KSessionStateMachine::roleName(m_sessionStateMachine.role()),
		QStringLiteral("peer_shutdown_finished"), nGeneration);
	m_pShutdownCoordinator->completePeer(nGeneration);
}

void KSessionCoordinator::handleStopWatchdog(quint64 nGeneration,
	bool bCapturePending,
	bool bPeerPending,
	qint64 nElapsedMs)
{
	if (!m_sessionStateMachine.isStopping()
		|| nGeneration != m_nStoppingGeneration)
	{
		return;
	}
	KSessionTraceLogger::write(roleToString(m_stopRole),
		QStringLiteral("session_stop_watchdog"),
		QStringLiteral("timeout"),
		-1,
		QStringLiteral("generation=%1 capturePending=%2 peerPending=%3 costMs=%4")
			.arg(nGeneration)
			.arg(bCapturePending ? 1 : 0)
			.arg(bPeerPending ? 1 : 0)
			.arg(nElapsedMs));
	m_pendingRequestType = NoPendingRequest;
	m_strPendingHost.clear();
	m_nPendingPort = 0;
	if (m_stopRole == ControlledSessionRole)
		m_pAccessSessionFlow->disconnectPeer();
	else
		m_pAccessSessionFlow->stop();
	if (!m_sessionStateMachine.markShutdownTimedOut())
		return;
	publishSessionState();
	reportSessionError(ShutdownSessionErrorDomain,
		ShutdownTimeoutSessionErrorCode, ShutdownSessionErrorStage,
		false,
		QStringLiteral("Shutdown timed out after %1 ms; capturePending=%2 peerPending=%3")
			.arg(nElapsedMs)
			.arg(bCapturePending ? 1 : 0)
			.arg(bPeerPending ? 1 : 0));
}

void KSessionCoordinator::finishStopping(quint64 nGeneration,
	bool bFinishedAfterTimeout)
{
	if (!m_sessionStateMachine.canCompleteShutdown()
		|| nGeneration != m_nStoppingGeneration)
	{
		return;
	}
	const bool bKeepListening = m_bStopKeepListening;
	const bool bReportError = m_bStopReportError;
	const bool bRecovering = m_bStopRecovering;
	const KSessionRole role = m_stopRole;
	const QString strReason = m_strStopReason;
	m_effectivePermissions = KPermissionScopes();
	m_bStopTeardownStarted = false;
	if (bFinishedAfterTimeout)
	{
		KSessionTraceLogger::write(roleToString(role),
			QStringLiteral("session_shutdown_late_complete"),
			QStringLiteral("completed"), -1,
			QStringLiteral("generation=%1").arg(nGeneration));
	}
	if (role == ControlledSessionRole && m_spPostSessionActionService != nullptr)
	{
		const KPrivacyOperationResult actionResult =
			m_spPostSessionActionService->consumeAfterTeardown(nGeneration);
		if (!actionResult.bSucceeded
			&& actionResult.strErrorCode != QStringLiteral("stale_generation"))
		{
			KSessionTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("post_session_action"), QStringLiteral("failed"), -1,
				QStringLiteral("generation=%1 errorCode=%2 technical=%3")
					.arg(nGeneration)
					.arg(actionResult.strErrorCode, actionResult.strTechnicalMessage));
		}
	}

	bool bListening = false;
	if (bKeepListening && role == ControlledSessionRole)
	{
		m_pAccessSessionFlow->disconnectPeer();
		const KPeerInitializationResult initializationResult =
			initializePeer(ControlledSessionRole);
		if (initializationResult.succeeded())
		{
			bListening = true;
			m_sessionStateMachine.finish(true);
			updateListeningAvailability(true, m_nListeningPort);
			publishSessionState();
		}
		else
		{
			m_pAccessSessionFlow->stop();
			m_sessionStateMachine.finish(false);
			publishSessionState();
			reportSessionError(WebRtcSessionErrorDomain,
				InitializationFailedSessionErrorCode, ShutdownSessionErrorStage,
				false, peerInitializationError(initializationResult));
		}
	}
	else
	{
		m_pAccessSessionFlow->stop();
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
	if (m_pPeerLifecycleController->rollbackPending())
		return;
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

KPeerInitializationResult KSessionCoordinator::initializePeer(KSessionRole role)
{
	m_bDeviceInfoRequested = false;
	const KPeerInitializationResult result = m_pPeerLifecycleController->initialize(role);
	m_nActivePeerGeneration = m_pPeerLifecycleController->generation();
	return result;
}

void KSessionCoordinator::handlePeerInitializationRollbackFinished(
	quint64 nGeneration, bool bFinishedAfterTimeout)
{
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("peer_initialization_rollback"),
		QStringLiteral("completed"), -1,
		QStringLiteral("generation=%1 afterTimeout=%2")
			.arg(nGeneration)
			.arg(bFinishedAfterTimeout ? 1 : 0));
	if (bFinishedAfterTimeout && m_sessionStateMachine.isShutdownTimedOut())
	{
		m_sessionStateMachine.finish(false);
		publishSessionState();
	}
	if (!bFinishedAfterTimeout)
		executePendingRequest();
}

void KSessionCoordinator::handlePeerInitializationRollbackTimeout(
	quint64 nGeneration, int nTimeoutMs)
{
	m_pendingRequestType = NoPendingRequest;
	m_strPendingHost.clear();
	m_nPendingPort = 0;
	if (!m_sessionStateMachine.markInitializationRollbackTimedOut())
		return;
	publishSessionState();
	reportSessionError(ShutdownSessionErrorDomain,
		ShutdownTimeoutSessionErrorCode, ShutdownSessionErrorStage,
		false,
		QStringLiteral("WebRTC initialization rollback timed out; generation=%1 timeoutMs=%2")
			.arg(nGeneration)
			.arg(nTimeoutMs));
}

void KSessionCoordinator::wirePeer()
{
	KRemotePeerTransport *pTransport = m_spRemotePeerTransport.get();
	connect(pTransport, &KRemotePeerTransport::signalingMessageReady,
		this, [this](quint64 nGeneration, const QString &strMessage)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			m_pAccessSessionFlow->sendSignalingMessage(strMessage);
		});
	connect(m_pAccessSessionFlow, &KAccessSessionFlow::messageReceived,
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
	connect(pTransport, &KRemotePeerTransport::terminalControlMessageReceived,
		this, [this](quint64 nGeneration, const KTerminalMessage &message)
		{
			if (nGeneration == m_nActivePeerGeneration
				&& hasPermission(TerminalPermissionScope))
				emit terminalControlMessageReceived(message);
			else if (nGeneration == m_nActivePeerGeneration)
				publishPermissionDenied(QStringLiteral("terminal_control_receive"));
		});
	connect(pTransport, &KRemotePeerTransport::terminalDataReceived,
		this, [this](quint64 nGeneration, const QByteArray &data)
		{
			if (nGeneration == m_nActivePeerGeneration
				&& hasPermission(TerminalPermissionScope))
				emit terminalDataReceived(data);
			else if (nGeneration == m_nActivePeerGeneration)
				publishPermissionDenied(QStringLiteral("terminal_data_receive"));
		});
	connect(pTransport, &KRemotePeerTransport::terminalChannelChanged,
		this, [this](quint64 nGeneration, bool bOpen)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			m_bTerminalChannelOpen = bOpen;
			if (!m_pCapabilitySessionFlow->isComplete())
				return;
			const bool bAvailable = bOpen
				&& hasPermission(TerminalPermissionScope)
				&& m_pCapabilitySessionFlow->negotiatedCapabilities().channels.contains(
					QStringLiteral("terminal"));
			if (bAvailable || m_bTerminalChannelStatePublished)
			{
				m_bTerminalChannelStatePublished = bAvailable;
				emit terminalChannelChanged(bAvailable);
			}
		});
	connect(pTransport, &KRemotePeerTransport::terminalLowWatermarkReached,
		this, [this](quint64 nGeneration)
		{
			if (nGeneration == m_nActivePeerGeneration)
				emit terminalLowWatermarkReached();
		});
	connect(pTransport, &KRemotePeerTransport::fileTransferLifecycleMessageReceived,
		this, [this](quint64 nGeneration,
			const KFileTransferLifecycleMessage &message)
		{
			if (nGeneration == m_nActivePeerGeneration
				&& hasPermission(FileTransferPermissionScope))
			{
				emit fileTransferLifecycleMessageReceived(message);
			}
			else if (nGeneration == m_nActivePeerGeneration)
			{
				publishPermissionDenied(QStringLiteral("file_transfer_lifecycle_receive"));
			}
		});
	connect(pTransport, &KRemotePeerTransport::fileTransferControlMessageReceived,
		this, [this](quint64 nGeneration,
			const KFileTransferControlMessage &message)
		{
			if (nGeneration == m_nActivePeerGeneration
				&& hasPermission(FileTransferPermissionScope)
				&& m_bFileTransferControlChannelOpen)
			{
				emit fileTransferControlMessageReceived(message);
			}
			else if (nGeneration == m_nActivePeerGeneration)
			{
				publishPermissionDenied(QStringLiteral("file_transfer_control_receive"));
			}
		});
	connect(pTransport, &KRemotePeerTransport::fileTransferDataReceived,
		this, [this](quint64 nGeneration, const QByteArray &data)
		{
			if (nGeneration == m_nActivePeerGeneration
				&& hasPermission(FileTransferPermissionScope)
				&& m_bFileTransferDataChannelOpen)
			{
				emit fileTransferDataReceived(data);
			}
			else if (nGeneration == m_nActivePeerGeneration)
			{
				publishPermissionDenied(QStringLiteral("file_transfer_data_receive"));
			}
		});
	connect(pTransport, &KRemotePeerTransport::fileTransferChannelsChanged,
		this, [this](quint64 nGeneration, bool bControlOpen, bool bDataOpen)
		{
			if (nGeneration != m_nActivePeerGeneration)
				return;
			m_bFileTransferControlChannelOpen = bControlOpen;
			m_bFileTransferDataChannelOpen = bDataOpen;
			if (!m_pCapabilitySessionFlow->isComplete())
				return;
			const QStringList &channels =
				m_pCapabilitySessionFlow->negotiatedCapabilities().channels;
			const bool bAvailable = bControlOpen && bDataOpen
				&& hasPermission(FileTransferPermissionScope)
				&& channels.contains(QStringLiteral("file-control"))
				&& channels.contains(QStringLiteral("file-data"));
			if (bAvailable || m_bFileTransferChannelStatePublished)
			{
				m_bFileTransferChannelStatePublished = bAvailable;
				emit fileTransferChannelsChanged(bAvailable, bAvailable);
			}
		});
	connect(pTransport, &KRemotePeerTransport::fileTransferLowWatermarkReached,
		this, [this](quint64 nGeneration)
		{
			if (nGeneration == m_nActivePeerGeneration)
				emit fileTransferLowWatermarkReached();
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
	if (!hasPermission(ViewScreenPermissionScope)
		|| frame.nWidth <= 0 || frame.nHeight <= 0 || !frame.hasPixels())
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

	if (!hasPermission(InputControlPermissionScope))
	{
		publishPermissionDenied(QStringLiteral("input_receive"));
		return;
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
	if (!hasPermission(ClipboardPermissionScope))
	{
		publishPermissionDenied(QStringLiteral("clipboard_receive"));
		return;
	}
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
	emit clipboardChannelChanged(bOpen
		&& hasPermission(ClipboardPermissionScope)
		&& m_pCapabilitySessionFlow->negotiatedCapabilities().bClipboardText);
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
		m_pCapabilitySessionFlow->reset();
		m_bDeviceInfoRequested = false;
		m_pSessionCommandDispatcher->failAll(QStringLiteral("session_channel_closed"));
		if (m_sessionStateMachine.isStopping())
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
	m_pCapabilitySessionFlow->begin(localCapabilities(), 3000);
	KSessionMessage capabilitiesMessage;
	capabilitiesMessage.type = CapabilitiesSessionMessageType;
	capabilitiesMessage.capabilities = m_pCapabilitySessionFlow->localCapabilities();
	sendSessionMessage(capabilitiesMessage);
}

void KSessionCoordinator::completeCapabilityNegotiation(
	const KNegotiatedCapabilities &capabilities)
{
	if (!m_pCapabilitySessionFlow->isComplete() || !m_bSessionChannelOpen)
		return;
	KPermissionScopes effectivePermissions = m_effectivePermissions;
	if (!capabilities.bClipboardText)
		effectivePermissions.setFlag(ClipboardPermissionScope, false);
	if (!capabilities.channels.contains(QStringLiteral("terminal")))
		effectivePermissions.setFlag(TerminalPermissionScope, false);
	if (!capabilities.channels.contains(QStringLiteral("file-control"))
		|| !capabilities.channels.contains(QStringLiteral("file-data")))
	{
		effectivePermissions.setFlag(FileTransferPermissionScope, false);
	}
	if (!capabilities.bKeyboard && !capabilities.bUnicodeText
		&& !capabilities.bMouseButtons && !capabilities.bMouseWheel)
	{
		effectivePermissions.setFlag(InputControlPermissionScope, false);
	}
	m_effectivePermissions = effectivePermissions;
	m_pMediaSessionController->setCapabilities(capabilities);
	m_bTerminalChannelStatePublished = m_bTerminalChannelOpen
		&& capabilities.channels.contains(QStringLiteral("terminal"));
	m_bFileTransferChannelStatePublished =
		m_bFileTransferControlChannelOpen
		&& m_bFileTransferDataChannelOpen
		&& capabilities.channels.contains(QStringLiteral("file-control"))
		&& capabilities.channels.contains(QStringLiteral("file-data"))
		&& effectivePermissions.testFlag(FileTransferPermissionScope);
	m_spRemotePeerTransport->setInputRealtimeEnabled(capabilities.bInputRealtime);
	if (!m_sessionStateMachine.markConnected())
		return;
	publishSessionState();
	emit sessionCapabilitiesChanged(capabilities);
	emit sessionPermissionsChanged(effectivePermissions);
	emit clipboardChannelChanged(m_bClipboardChannelOpen
		&& hasPermission(ClipboardPermissionScope)
		&& capabilities.bClipboardText);
	emit terminalChannelChanged(m_bTerminalChannelStatePublished);
	emit fileTransferChannelsChanged(
		m_bFileTransferChannelStatePublished,
		m_bFileTransferChannelStatePublished);
	const quint64 nGeneration = m_sessionStateMachine.generation();
	if (m_sessionStateMachine.role() == ControlledSessionRole)
	{
		if (m_spPrivacyModeService != nullptr)
			m_spPrivacyModeService->beginSession(nGeneration);
		if (m_spPostSessionActionService != nullptr)
			m_spPostSessionActionService->beginSession(nGeneration);
	}
	else
	{
		KPrivacyModeStatus privacyStatus;
		privacyStatus.nGeneration = nGeneration;
		emit privacyModeStatusChanged(privacyStatus);
		KPostSessionActionStatus actionStatus;
		actionStatus.nGeneration = nGeneration;
		emit postSessionActionStatusChanged(actionStatus);
	}
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
	capabilities.bInputRealtime = true;
	capabilities.supportedCodecs = { QStringLiteral("h264") };
	capabilities.supportedChannels = {
		QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input"),
		QStringLiteral("input-realtime"), QStringLiteral("clipboard"),
		QStringLiteral("file-control"), QStringLiteral("file-data")
	};
	capabilities.supportedPrivacyModes = { QStringLiteral("disabled") };
	if (hasPermission(ViewScreenPermissionScope)
		&& hasPermission(InputControlPermissionScope))
	{
		if (m_sessionStateMachine.role() == ControllerSessionRole)
		{
			capabilities.supportedPrivacyModes.append(QStringLiteral("privacyoverlay"));
			capabilities.supportedPrivacyModes.append(QStringLiteral("displayoff"));
		}
		else if (m_spPrivacyModeService != nullptr)
		{
			capabilities.supportedPrivacyModes =
				m_spPrivacyModeService->supportedModes();
		}
	}
	capabilities.bPostSessionLock = hasPermission(InputControlPermissionScope)
		&& (m_sessionStateMachine.role() == ControllerSessionRole
			|| (m_spPostSessionActionService != nullptr
				&& m_spPostSessionActionService->isSupported()));
	const bool bTerminalAvailable = m_sessionStateMachine.role() == ControllerSessionRole
		? m_bControllerTerminalCapabilityAvailable
		: m_bControlledTerminalCapabilityAvailable;
	if (bTerminalAvailable)
		capabilities.supportedChannels.append(QStringLiteral("terminal"));
	if (!hasPermission(ClipboardPermissionScope))
	{
		capabilities.bClipboardText = false;
		capabilities.supportedChannels.removeAll(QStringLiteral("clipboard"));
	}
	if (!hasPermission(TerminalPermissionScope))
		capabilities.supportedChannels.removeAll(QStringLiteral("terminal"));
	if (!hasPermission(FileTransferPermissionScope))
	{
		capabilities.supportedChannels.removeAll(QStringLiteral("file-control"));
		capabilities.supportedChannels.removeAll(QStringLiteral("file-data"));
	}
	if (!hasPermission(InputControlPermissionScope))
	{
		capabilities.bInputRealtime = false;
		capabilities.bKeyboard = false;
		capabilities.bUnicodeText = false;
		capabilities.bMouseButtons = false;
		capabilities.bMouseWheel = false;
	}
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
	const KCapabilityNegotiationResult negotiation =
		m_pCapabilitySessionFlow->receive(message.capabilities);
	if (!negotiation.succeeded())
	{
		KSessionMessage rejection;
		rejection.type = CapabilityRejectedSessionMessageType;
		rejection.strReason = QStringLiteral("incompatible_capabilities");
		sendSessionMessage(rejection);
		finishSession(ConnectFailedSessionEndReason, QStringLiteral("incompatible_protocol"),
			m_sessionStateMachine.shouldKeepListening(), false, false);
		reportSessionError(ProtocolSessionErrorDomain,
			IncompatibleProtocolSessionErrorCode, NegotiationSessionErrorStage,
			false, negotiation.strTechnicalMessage);
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
			negotiation.strTechnicalMessage);
	}
	completeCapabilityNegotiation(negotiation.capabilities);
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

KProtocolHandlerResult KSessionCoordinator::handleSetPrivacyModeMessage(
	const KSessionMessage &message)
{
	const KSessionState state = m_sessionStateMachine.state();
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| (state != StreamingSessionState && state != ReconnectingSessionState)
		|| m_spPrivacyModeService == nullptr)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("invalid_session_state"),
			QStringLiteral("Privacy mode cannot be changed in the current state"));
	}
	if (!hasPermission(ViewScreenPermissionScope)
		|| !hasPermission(InputControlPermissionScope))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerPermissionDenied,
			QStringLiteral("permission_denied"),
			QStringLiteral("Privacy mode requires view and input permissions"));
	}
	QString strMode;
	if (message.privacyMode == DisabledPrivacyMode)
		strMode = QStringLiteral("disabled");
	else if (message.privacyMode == PrivacyOverlayPrivacyMode)
		strMode = QStringLiteral("privacyoverlay");
	else if (message.privacyMode == DisplayOffPrivacyMode)
		strMode = QStringLiteral("displayoff");
	if (strMode.isEmpty()
		|| !m_pCapabilitySessionFlow->negotiatedCapabilities()
			.supportedPrivacyModes.contains(strMode))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
			QStringLiteral("unsupported_mode"),
			QStringLiteral("The requested privacy mode was not negotiated"));
	}
	const KPrivacyOperationResult result = m_spPrivacyModeService->setMode(
		message.privacyMode, message.strRequestId, m_sessionStateMachine.generation());
	if (!result.bSucceeded)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
			result.strErrorCode, result.strTechnicalMessage);
	}
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handlePrivacyModeStateMessage(
	const KSessionMessage &message)
{
	const KSessionState state = m_sessionStateMachine.state();
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| (state != ConnectedSessionState
			&& state != StreamingSessionState
			&& state != ReconnectingSessionState))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("invalid_session_state"),
			QStringLiteral("Privacy state arrived outside an active controller session"));
	}
	KPrivacyModeStatus status = message.privacyModeStatus;
	status.nGeneration = m_sessionStateMachine.generation();
	emit privacyModeStatusChanged(status);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handleSetPostSessionActionMessage(
	const KSessionMessage &message)
{
	const KSessionState state = m_sessionStateMachine.state();
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| (state != StreamingSessionState && state != ReconnectingSessionState)
		|| m_spPostSessionActionService == nullptr)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("invalid_session_state"),
			QStringLiteral("Post-session action cannot be changed now"));
	}
	if (!hasPermission(InputControlPermissionScope))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerPermissionDenied,
			QStringLiteral("permission_denied"),
			QStringLiteral("Post-session lock requires input permission"));
	}
	if (message.postSessionAction == UnknownPostSessionAction
		|| (message.postSessionAction == LockWorkstationPostSessionAction
			&& !m_pCapabilitySessionFlow->negotiatedCapabilities().bPostSessionLock))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
			QStringLiteral("unsupported_action"),
			QStringLiteral("The requested post-session action was not negotiated"));
	}
	const KPrivacyOperationResult result = m_spPostSessionActionService->setAction(
		message.postSessionAction, message.strRequestId,
		m_sessionStateMachine.generation());
	if (!result.bSucceeded)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
			result.strErrorCode, result.strTechnicalMessage);
	}
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KSessionCoordinator::handlePostSessionActionStateMessage(
	const KSessionMessage &message)
{
	const KSessionState state = m_sessionStateMachine.state();
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| (state != ConnectedSessionState
			&& state != StreamingSessionState
			&& state != ReconnectingSessionState))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("invalid_session_state"),
			QStringLiteral("Post-session state arrived outside an active session"));
	}
	KPostSessionActionStatus status = message.postSessionActionStatus;
	status.nGeneration = m_sessionStateMachine.generation();
	emit postSessionActionStatusChanged(status);
	return KProtocolHandlerResult::success();
}

void KSessionCoordinator::publishPrivacyModeStatus(const KPrivacyModeStatus &status)
{
	emit privacyModeStatusChanged(status);
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| !m_bSessionChannelOpen || !m_pCapabilitySessionFlow->isComplete())
	{
		return;
	}
	KSessionMessage message;
	message.type = PrivacyModeStateSessionMessageType;
	message.strRequestId = status.strRequestId;
	message.privacyModeStatus = status;
	sendSessionMessage(message);
}

void KSessionCoordinator::publishPostSessionActionStatus(
	const KPostSessionActionStatus &status)
{
	emit postSessionActionStatusChanged(status);
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| !m_bSessionChannelOpen || !m_pCapabilitySessionFlow->isComplete())
	{
		return;
	}
	KSessionMessage message;
	message.type = PostSessionActionStateSessionMessageType;
	message.strRequestId = status.strRequestId;
	message.postSessionActionStatus = status;
	sendSessionMessage(message);
}

void KSessionCoordinator::handleCapabilityTimeout()
{
	if (m_sessionStateMachine.state() != NegotiatingSessionState
		|| m_pCapabilitySessionFlow->isComplete())
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
		m_pSessionCommandDispatcher->handleIncoming(message,
			m_sessionStateMachine.generation());
		return;
	}
	if (!m_pCapabilitySessionFlow->isComplete()
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
	const KSessionIncomingDispatchResult dispatchResult =
		m_pSessionCommandDispatcher->handleIncoming(message,
			m_sessionStateMachine.generation());
	if (dispatchResult.bEndSessionAccepted)
	{
		finishSession(RemoteStopSessionEndReason,
			message.strReason,
			m_sessionStateMachine.shouldKeepListening(),
			false,
			m_sessionStateMachine.role() == ControllerSessionRole);
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
	const KSessionCommandTransmitResult result = sendDeviceInfoMessage();
	if (!result.bAccepted)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
			QStringLiteral("Unable to send device information: %1")
				.arg(result.strErrorCode));
	}
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
	m_pMediaSessionController->startCapture(m_sessionStateMachine.generation());
	m_sessionStateMachine.beginStreaming();
	if (m_spPostSessionActionService != nullptr)
		m_spPostSessionActionService->markStreaming(m_sessionStateMachine.generation());
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
	m_pMediaSessionController->stopCapture(m_sessionStateMachine.generation());
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
	m_pAccessSessionFlow->setConnected(true);

	if (!m_sessionStateMachine.beginAuthenticatingIdentity())
		return;
	m_pAccessSessionFlow->beginOutgoing(m_sessionStateMachine.generation(),
		m_spDeviceInfoProvider != nullptr
		? m_spDeviceInfoProvider->deviceName()
		: QStringLiteral("Windows device"));
	publishSessionState();
}

void KSessionCoordinator::handleOutgoingConnectionFailed(const QString &strMessage)
{
	m_pAccessSessionFlow->setConnected(false);
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

	if (!m_sessionStateMachine.beginAuthenticatingIdentity())
		return;
	m_pAccessSessionFlow->setConnected(true);
	m_pAccessSessionFlow->beginIncoming(strSourceAddress,
		m_sessionStateMachine.generation(),
		m_spDeviceInfoProvider != nullptr
			? m_spDeviceInfoProvider->deviceName()
			: QStringLiteral("Windows device"));
	updateListeningAvailability(false);
	publishSessionState();
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("access"),
		QStringLiteral("connection_received"),
		-1,
		QStringLiteral("source=%1 sourcePort=%2 generation=%3")
			.arg(strSourceAddress)
			.arg(nSourcePort)
			.arg(m_sessionStateMachine.generation()));
}

void KSessionCoordinator::handleSignalingMessage(const QString &strMessage)
{
	KProtocolRouteContext context;
	context.nRole = static_cast<int>(m_sessionStateMachine.role());
	context.nState = static_cast<int>(m_sessionStateMachine.state());
	context.nGeneration = m_sessionStateMachine.generation();
	const KProtocolRouteResult result = m_protocolRouter.route(
		SignalingProtocolChannel, strMessage, context);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("protocol_route"),
		QStringLiteral("signaling"),
		strMessage.toUtf8().size(),
		QStringLiteral("type=%1 routeStatus=%2 handlerStatus=%3")
			.arg(result.envelope.strType)
			.arg(static_cast<int>(result.status))
			.arg(static_cast<int>(result.handlerResult.status)));
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
	return m_pAccessSessionFlow->handleAccessMessage(message,
		m_sessionStateMachine.generation())
		? KProtocolHandlerResult::success()
		: KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Access message does not match the active request"));
}

KProtocolHandlerResult KSessionCoordinator::handleWebRtcSignalingEnvelope(
	const KProtocolEnvelope &envelope)
{
	QString strError;
	KWebRtcSignalingMessage message;
	if (!KWebRtcSignalingMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	const KSessionRole role = m_sessionStateMachine.role();
	if ((message.type == OfferWebRtcSignalingMessageType
			&& role != ControlledSessionRole)
		|| (message.type == AnswerWebRtcSignalingMessageType
			&& role != ControllerSessionRole))
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("TLS signaling direction is invalid"));
	}
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
	const bool bWasIdentityHandshake = m_sessionStateMachine.isAuthenticatingIdentity()
		|| m_sessionStateMachine.isPairing();
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
		m_pAccessSessionFlow->rejectIncoming(QStringLiteral("invalid_request"), false);
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
	reportSessionError(ProtocolSessionErrorDomain, code,
		(bWasAwaitingApproval || bWasIdentityHandshake)
			? ApprovalSessionErrorStage : NegotiationSessionErrorStage,
		false, strError.isEmpty()
			? QStringLiteral("Unsupported or invalid signaling message") : strError);
}

void KSessionCoordinator::handleOutgoingAccessRejected(const QString &strReason)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| (!m_sessionStateMachine.isAwaitingApproval()
			&& !m_sessionStateMachine.isAuthenticatingIdentity()
			&& !m_sessionStateMachine.isPairing()))
		return;
	finishSession(ConnectFailedSessionEndReason, strReason, false, false, false);
	KSessionErrorCode code = ApprovalRejectedSessionErrorCode;
	KSessionErrorDomain domain = AccessSessionErrorDomain;
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
	reportSessionError(domain, code,
		ApprovalSessionErrorStage, bRetryable,
		QStringLiteral("Access rejected: %1").arg(strReason));
}

void KSessionCoordinator::handleIncomingSecurityRejected(
	const KSecurityStatus &status)
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| (!m_sessionStateMachine.isAwaitingApproval()
			&& !m_sessionStateMachine.isAuthenticatingIdentity()
			&& !m_sessionStateMachine.isPairing()))
	{
		return;
	}
	const KSessionError error = KSecuritySessionErrorMapper::map(status);
	reportSessionError(error.domain, error.code, error.stage,
		error.bRetryable, error.strTechnicalMessage);
	m_sessionStateMachine.rejectConnection();
	m_pAccessSessionFlow->disconnectPeer();
	updateListeningAvailability(true, m_nListeningPort);
	publishSessionState();
}

void KSessionCoordinator::handleOutgoingSecurityRejected(
	const KSecurityStatus &status)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole
		|| (!m_sessionStateMachine.isAwaitingApproval()
			&& !m_sessionStateMachine.isAuthenticatingIdentity()
			&& !m_sessionStateMachine.isPairing()))
	{
		return;
	}
	finishSession(ConnectFailedSessionEndReason, status.strProtocolReason,
		false, false, false);
	const KSessionError error = KSecuritySessionErrorMapper::map(status);
	reportSessionError(error.domain, error.code, error.stage,
		error.bRetryable, error.strTechnicalMessage);
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
	m_pAccessSessionFlow->setConnected(false);
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
	const quint64 nGeneration = m_sessionStateMachine.generation();
	m_pRecoveryController->begin(nGeneration, kReconnectTimeoutMs);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("session_recovery_start"),
		QStringLiteral("ice_disconnected"),
		-1,
		QStringLiteral("generation=%1 signalingAvailable=%2 timeoutMs=%3")
			.arg(nGeneration)
			.arg(m_pAccessSessionFlow->isConnected() ? 1 : 0)
			.arg(kReconnectTimeoutMs));

	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	if (!m_pAccessSessionFlow->isConnected())
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

	const qint64 nCostMs = m_pRecoveryController->complete(
		m_sessionStateMachine.generation());
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

void KSessionCoordinator::handleReconnectTimeout(quint64 nGeneration)
{
	if (!m_sessionStateMachine.isReconnecting()
		|| !m_pRecoveryController->isActive(nGeneration)
		|| nGeneration != m_sessionStateMachine.generation())
	{
		return;
	}

	finishSession(DisconnectTimeoutSessionEndReason,
		QString(),
		m_sessionStateMachine.shouldKeepListening(),
		false,
		true);
}

KSessionCommandTransmitResult KSessionCoordinator::sendDeviceInfoMessage()
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
	return transmitSessionMessage(message);
}

bool KSessionCoordinator::hasPermission(KPermissionScope permission) const
{
	return m_effectivePermissions.testFlag(permission);
}

void KSessionCoordinator::publishPermissionDenied(
	const QString &strOperation) const
{
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("security_drop"), QStringLiteral("permission_denied"),
		-1, QStringLiteral("operation=%1 generation=%2")
			.arg(strOperation)
			.arg(m_sessionStateMachine.generation()));
}
