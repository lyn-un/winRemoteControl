#include "session/accesssessionflow.h"

#include "common/sessiontracelogger.h"
#include "core/transport/signalingtransport.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/trusteddevicestore.h"
#include "session/accessapprovalcontroller.h"

#include <QtCore/QDateTime>

namespace
{
	constexpr int kInitialApprovalResponseTimeoutMs = 5000;
	constexpr int kApprovalResponseGraceMs = 5000;
}

KAccessSessionFlow::KAccessSessionFlow(KSignalingTransport *pTransport,
	KDeviceIdentityProvider *pIdentityProvider,
	KTrustedDeviceStore *pTrustedDeviceStore,
	QObject *pParent)
	: QObject(pParent)
	, m_pTransport(pTransport)
	, m_pIdentityProvider(pIdentityProvider)
	, m_pApprovalController(new KAccessApprovalController(this))
	, m_pAuthenticationFlow(new KDeviceAuthenticationFlow(
		pIdentityProvider, pTrustedDeviceStore, pTransport, this))
{
	Q_ASSERT(m_pTransport != nullptr);
	KAccessMessage busy;
	busy.type = ServerBusyAccessMessageType;
	m_pTransport->setServerBusyMessage(KAccessMessageCodec::encode(busy));
	connect(m_pTransport, &KSignalingTransport::messageReceived,
		this, &KAccessSessionFlow::messageReceived);
	connect(m_pTransport, &KSignalingTransport::stateChanged,
		this, &KAccessSessionFlow::stateChanged);
	connect(m_pTransport, &KSignalingTransport::signalingError,
		this, &KAccessSessionFlow::signalingError);
	connect(m_pTransport, &KSignalingTransport::outgoingConnectionEstablished,
		this, &KAccessSessionFlow::outgoingConnectionEstablished);
	connect(m_pTransport, &KSignalingTransport::outgoingConnectionFailed,
		this, &KAccessSessionFlow::outgoingConnectionFailed);
	connect(m_pTransport, &KSignalingTransport::incomingConnectionEstablished,
		this, &KAccessSessionFlow::incomingConnectionEstablished);
	connect(m_pTransport, &KSignalingTransport::connectionLost,
		this, &KAccessSessionFlow::connectionLost);
	connect(m_pTransport, &KSignalingTransport::secureChannelEstablished,
		this, [this](const KTlsPeerIdentity &peer)
		{ m_pAuthenticationFlow->setSecurePeerIdentity(peer); });
	connect(m_pApprovalController, &KAccessApprovalController::timedOut,
		this, &KAccessSessionFlow::handleApprovalTimeout);
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::messageReady,
		this, [this](const KTlsPairingMessage &message)
		{ m_pTransport->sendMessage(KTlsPairingMessageCodec::encode(message)); });
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::pairingRequested,
		this, &KAccessSessionFlow::pairingRequested);
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::pairingCleared,
		this, &KAccessSessionFlow::pairingCleared);
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::authenticationSucceeded,
		this, &KAccessSessionFlow::handleAuthenticationSucceeded);
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::authenticationRejected,
		this, &KAccessSessionFlow::handleAuthenticationRejected);
}

void KAccessSessionFlow::setApplicationSettings(
	const KApplicationSettings &settings)
{
	m_settings = settings;
	m_pAuthenticationFlow->setApprovalTimeoutSeconds(settings.nApprovalTimeoutSeconds);
}

bool KAccessSessionFlow::startListening(quint16 nPort, QString *pErrorMessage)
{
	m_bConnected = false;
	if (!ensureSecureIdentity(pErrorMessage))
		return false;
	if (!m_pTransport->startServer(nPort, pErrorMessage))
		return false;
	m_nListeningPort = nPort;
	return true;
}

void KAccessSessionFlow::connectToHost(const QString &strHost, quint16 nPort)
{
	clearApproval(QStringLiteral("new_connection"));
	m_pAuthenticationFlow->cancel(QStringLiteral("cancelled"), false, false);
	m_strLastHost = strHost;
	m_nLastPort = nPort;
	m_bConnected = false;
	m_pAuthenticationFlow->setSecurePeerIdentity(KTlsPeerIdentity());
	m_pTransport->stop();
	QString strError;
	if (!ensureSecureIdentity(&strError))
	{
		emit outgoingConnectionFailed(strError);
		return;
	}
	m_pTransport->connectToHost(strHost, nPort);
}

bool KAccessSessionFlow::ensureSecureIdentity(QString *pErrorMessage)
{
	if (m_pIdentityProvider == nullptr)
	{
		if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
			*pErrorMessage = QStringLiteral("Device identity is unavailable");
		return false;
	}
	if (!m_pIdentityProvider->certificate().isValid()
		&& !m_pIdentityProvider->initialize(pErrorMessage))
	{
		return false;
	}
	return m_pTransport->setIdentityProvider(m_pIdentityProvider, pErrorMessage);
}

void KAccessSessionFlow::disconnectPeer()
{
	m_bConnected = false;
	m_pAuthenticationFlow->setSecurePeerIdentity(KTlsPeerIdentity());
	m_pTransport->disconnectPeer();
}

void KAccessSessionFlow::stop()
{
	clearApproval(QStringLiteral("stopped"));
	m_pAuthenticationFlow->cancel(QStringLiteral("cancelled"), false, false);
	m_bConnected = false;
	m_pAuthenticationFlow->setSecurePeerIdentity(KTlsPeerIdentity());
	m_pTransport->stop();
}

void KAccessSessionFlow::beginOutgoing(
	quint64 nGeneration,
	const QString &strDeviceName)
{
	const KAccessApprovalRequest request = m_pApprovalController->beginOutgoing(
		nGeneration, m_settings.nApprovalTimeoutSeconds * 1000 + kInitialApprovalResponseTimeoutMs);
	m_strLocalDeviceName = strDeviceName;
	QString strError;
	if (!m_pAuthenticationFlow->beginOutgoing(request.strRequestId,
		nGeneration, strDeviceName, KPermissionScopes::fromInt(kAllPermissionScopeBits),
		&strError))
	{
		const QString strReason = strError.startsWith(
			QStringLiteral("trust_store_tampered"))
			? QStringLiteral("trust_store_tampered")
			: QStringLiteral("identity_unavailable");
		clearApproval(strReason);
		emit outgoingAccessRejected(strReason);
		return;
	}
	KSessionTraceLogger::write(QStringLiteral("controller"), QStringLiteral("access"),
		QStringLiteral("request_sent"), -1,
		QStringLiteral("requestId=%1 generation=%2")
			.arg(request.strRequestId).arg(request.nGeneration));
}

void KAccessSessionFlow::beginIncoming(
	const QString &strSourceAddress,
	quint64 nGeneration,
	const QString &strDeviceName)
{
	m_pApprovalController->beginIncoming(strSourceAddress, nGeneration,
		kInitialApprovalResponseTimeoutMs);
	m_strLocalDeviceName = strDeviceName;
	QString strError;
	if (!m_pAuthenticationFlow->beginIncoming(strSourceAddress, nGeneration,
		strDeviceName,
		&strError))
	{
		emit incomingAccessRejected(strError.startsWith(
			QStringLiteral("trust_store_tampered"))
			? QStringLiteral("trust_store_tampered")
			: QStringLiteral("identity_unavailable"));
	}
}

bool KAccessSessionFlow::handleTlsPairingMessage(
	const KTlsPairingMessage &message,
	quint64 nGeneration)
{
	return m_pAuthenticationFlow->handleMessage(message, nGeneration);
}

void KAccessSessionFlow::respondPairing(const QString &strRequestId,
	bool bAccepted,
	KPermissionScopes permissions)
{
	m_pAuthenticationFlow->respondPairing(strRequestId, bAccepted, permissions);
}

bool KAccessSessionFlow::handleAccessMessage(
	const KAccessMessage &message,
	quint64 nGeneration)
{
	const KAccessApprovalRequest current = m_pApprovalController->request();
	if (current.side == IncomingAccessApprovalSide)
	{
		if (message.type == RejectedAccessMessageType
			&& m_pApprovalController->matches(message.strRequestId, nGeneration))
		{
			rejectIncoming(QStringLiteral("cancelled"), false);
			return true;
		}
		if (message.type != RequestAccessMessageType
			|| m_pApprovalController->hasRequestId())
		{
			return true;
		}
		if (!m_pApprovalController->receiveIncomingRequest(message.strRequestId,
			message.strDeviceName, m_settings.nApprovalTimeoutSeconds * 1000))
		{
			return false;
		}
		const KAccessApprovalRequest &request = m_pApprovalController->request();
		const KDeviceAuthenticationContext authentication =
			m_pAuthenticationFlow->context();
		if (!m_pAuthenticationFlow->isAuthenticated()
			|| authentication.strRequestId != message.strRequestId)
		{
			return false;
		}
		emit incomingAccessObserved(request.strDeviceName, request.strSourceAddress);
		KIncomingAccessDecision decision =
			KAccessApprovalController::incomingDecision(m_settings);
		if (decision == AcceptIncomingAccessDecision
			&& (!authentication.bTrustedDevice
				|| !authentication.bRequestWithinTrust))
		{
			decision = AskIncomingAccessDecision;
		}
		switch (decision)
		{
		case DisabledIncomingAccessDecision:
			rejectIncoming(QStringLiteral("remote_access_disabled"), true);
			return true;
		case DenyIncomingAccessDecision:
			rejectIncoming(QStringLiteral("user_rejected"), true);
			return true;
		case AcceptIncomingAccessDecision:
			acceptIncoming();
			return true;
		case AskIncomingAccessDecision:
		default:
			break;
		}
		KAccessMessage pending;
		pending.type = PendingAccessMessageType;
		pending.strRequestId = request.strRequestId;
		pending.nTimeoutSeconds = m_settings.nApprovalTimeoutSeconds;
		sendAccessMessage(pending);
		const qint64 nExpiresAtMs = QDateTime::currentMSecsSinceEpoch()
			+ m_settings.nApprovalTimeoutSeconds * 1000LL;
		emit incomingAccessRequest(request.strRequestId, request.strDeviceName,
			request.strSourceAddress, nExpiresAtMs);
		return true;
	}

	if (!m_pApprovalController->matches(message.strRequestId, nGeneration))
		return true;
	if (message.type == PendingAccessMessageType)
	{
		m_pApprovalController->extendOutgoingTimeout(message.strRequestId,
			message.nTimeoutSeconds * 1000 + kApprovalResponseGraceMs);
		return true;
	}
	if (message.type == AcceptedAccessMessageType)
	{
		m_pApprovalController->stopTimeout();
		clearApproval(QStringLiteral("accepted"));
		emit outgoingAccessAccepted();
		return true;
	}
	if (message.type == RejectedAccessMessageType)
	{
		const QString strReason = message.strReason;
		clearApproval(strReason);
		emit outgoingAccessRejected(strReason);
		return true;
	}
	return true;
}

void KAccessSessionFlow::handleAuthenticationSucceeded(
	const KDeviceAuthenticationContext &context)
{
	emit identityAuthenticated(context);
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side != OutgoingAccessApprovalSide)
		return;
	m_pApprovalController->extendOutgoingTimeout(request.strRequestId,
		kInitialApprovalResponseTimeoutMs);
	KAccessMessage message;
	message.type = RequestAccessMessageType;
	message.strRequestId = context.strRequestId;
	message.strDeviceName = m_strLocalDeviceName;
	sendAccessMessage(message);
}

void KAccessSessionFlow::handleAuthenticationRejected(const QString &strReason)
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	clearApproval(strReason);
	if (request.side == IncomingAccessApprovalSide)
		emit incomingAccessRejected(strReason);
	else if (request.side == OutgoingAccessApprovalSide)
		emit outgoingAccessRejected(strReason);
}

void KAccessSessionFlow::respondIncoming(
	const QString &strRequestId,
	bool bAccepted)
{
	if (!m_pApprovalController->matches(strRequestId,
		m_pApprovalController->request().nGeneration)
		|| m_pApprovalController->request().side != IncomingAccessApprovalSide)
	{
		return;
	}
	if (bAccepted)
		acceptIncoming();
	else
		rejectIncoming(QStringLiteral("user_rejected"), true);
}

void KAccessSessionFlow::rejectIncoming(
	const QString &strReason,
	bool bNotifyRemote)
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side != IncomingAccessApprovalSide)
		return;
	if (bNotifyRemote && !request.strRequestId.isEmpty())
	{
		KAccessMessage rejected;
		rejected.type = RejectedAccessMessageType;
		rejected.strRequestId = request.strRequestId;
		rejected.strReason = strReason;
		sendAccessMessage(rejected);
	}
	clearApproval(strReason);
	emit incomingAccessRejected(strReason);
}

void KAccessSessionFlow::cancelApproval(
	const QString &strReason,
	bool bNotifyRemote,
	bool bEmitOutcome)
{
	if (m_pAuthenticationFlow->isActive())
		m_pAuthenticationFlow->cancel(strReason, bNotifyRemote, false);
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side == NoAccessApprovalSide)
		return;
	if (bNotifyRemote && !request.strRequestId.isEmpty())
	{
		KAccessMessage rejected;
		rejected.type = RejectedAccessMessageType;
		rejected.strRequestId = request.strRequestId;
		rejected.strReason = strReason;
		sendAccessMessage(rejected);
	}
	clearApproval(strReason);
	if (!bEmitOutcome)
		return;
	if (request.side == IncomingAccessApprovalSide)
		emit incomingAccessRejected(strReason);
	else
		emit outgoingAccessRejected(strReason);
}

void KAccessSessionFlow::clearApproval(const QString &strReason)
{
	const KAccessApprovalRequest request = m_pApprovalController->clear();
	if (request.side == IncomingAccessApprovalSide && !request.strRequestId.isEmpty())
		emit incomingAccessRequestCleared(request.strRequestId, strReason);
}

bool KAccessSessionFlow::hasApproval() const
{
	return m_pApprovalController->request().side != NoAccessApprovalSide;
}

void KAccessSessionFlow::sendSignalingMessage(const QString &strMessage)
{
	m_pTransport->sendMessage(strMessage);
}

void KAccessSessionFlow::setConnected(bool bConnected) { m_bConnected = bConnected; }
bool KAccessSessionFlow::isConnected() const { return m_bConnected; }

bool KAccessSessionFlow::matchesEndpoint(const QString &strHost, quint16 nPort) const
{
	return m_strLastHost.compare(strHost, Qt::CaseInsensitive) == 0
		&& m_nLastPort == nPort;
}

bool KAccessSessionFlow::hasLastEndpoint() const
{
	return !m_strLastHost.isEmpty() && m_nLastPort != 0;
}

QString KAccessSessionFlow::lastHost() const { return m_strLastHost; }
quint16 KAccessSessionFlow::lastPort() const { return m_nLastPort; }
quint16 KAccessSessionFlow::listeningPort() const { return m_nListeningPort; }

KDeviceAuthenticationContext KAccessSessionFlow::authenticationContext() const
{
	return m_pAuthenticationFlow->context();
}

void KAccessSessionFlow::clearLastEndpoint()
{
	m_strLastHost.clear();
	m_nLastPort = 0;
}

void KAccessSessionFlow::sendAccessMessage(const KAccessMessage &message)
{
	m_pTransport->sendMessage(KAccessMessageCodec::encode(message));
}

void KAccessSessionFlow::acceptIncoming()
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side != IncomingAccessApprovalSide || request.strRequestId.isEmpty())
		return;
	KAccessMessage accepted;
	accepted.type = AcceptedAccessMessageType;
	accepted.strRequestId = request.strRequestId;
	sendAccessMessage(accepted);
	clearApproval(QStringLiteral("accepted"));
	emit incomingAccessAccepted();
}

void KAccessSessionFlow::handleApprovalTimeout(
	const QString &strRequestId,
	quint64 nGeneration)
{
	if (!m_pApprovalController->matches(strRequestId, nGeneration))
		return;
	if (m_pApprovalController->request().side == IncomingAccessApprovalSide)
		rejectIncoming(QStringLiteral("timeout"), true);
	else
	{
		clearApproval(QStringLiteral("timeout"));
		emit outgoingAccessRejected(QStringLiteral("timeout"));
	}
}
