#include "session/accesssessionflow.h"

#include "common/sessiontracelogger.h"
#include "core/transport/signalingtransport.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/admissioncontroller.h"
#include "core/security/trusteddevicestore.h"
#include "session/accessapprovalcontroller.h"
#include "session/securitysessioncontroller.h"

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
	, m_upAdmissionController(std::make_unique<KAdmissionController>())
	, m_pIdentityProvider(pIdentityProvider)
	, m_pApprovalController(new KAccessApprovalController(this))
	, m_pSecurityController(new KSecuritySessionController(
		pTransport, pIdentityProvider, pTrustedDeviceStore, this))
{
	Q_ASSERT(m_pTransport != nullptr);
	m_pTransport->setAdmissionController(m_upAdmissionController.get());
	m_pSecurityController->setAdmissionController(m_upAdmissionController.get());
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
	connect(m_pApprovalController, &KAccessApprovalController::timedOut,
		this, &KAccessSessionFlow::handleApprovalTimeout);
	connect(m_pSecurityController, &KSecuritySessionController::pairingRequested,
		this, &KAccessSessionFlow::pairingRequested);
	connect(m_pSecurityController, &KSecuritySessionController::pairingCleared,
		this, &KAccessSessionFlow::pairingCleared);
	connect(m_pSecurityController,
		&KSecuritySessionController::authenticationSucceeded,
		this, &KAccessSessionFlow::handleAuthenticationSucceeded);
	connect(m_pSecurityController,
		&KSecuritySessionController::authenticationRejected,
		this, &KAccessSessionFlow::handleAuthenticationRejected);
}

void KAccessSessionFlow::setApplicationSettings(
	const KApplicationSettings &settings)
{
	m_settings = settings;
	m_pSecurityController->setApprovalTimeoutSeconds(settings.nApprovalTimeoutSeconds);
}

bool KAccessSessionFlow::startListening(quint16 nPort, QString *pErrorMessage)
{
	m_bConnected = false;
	if (!ensureSecureIdentity(pErrorMessage))
		return false;
	if (!m_pTransport->startServer(nPort, pErrorMessage))
		return false;
	m_nListeningPort = m_pTransport->listeningPort();
	if (m_nListeningPort == 0)
		m_nListeningPort = nPort;
	return true;
}

void KAccessSessionFlow::connectToHost(const QString &strHost, quint16 nPort)
{
	clearApproval(QStringLiteral("new_connection"));
	m_pSecurityController->cancel(QStringLiteral("cancelled"), false, false);
	m_strLastHost = strHost;
	m_nLastPort = nPort;
	m_bConnected = false;
	m_pSecurityController->clearPeerIdentity();
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
	m_pSecurityController->clearPeerIdentity();
	m_pTransport->disconnectPeer();
}

void KAccessSessionFlow::stop()
{
	clearApproval(QStringLiteral("stopped"));
	m_pSecurityController->cancel(QStringLiteral("cancelled"), false, false);
	m_bConnected = false;
	m_pSecurityController->clearPeerIdentity();
	m_pTransport->stop();
}

void KAccessSessionFlow::beginOutgoing(
	quint64 nGeneration,
	const QString &strDeviceName)
{
	const KAccessApprovalRequest request = m_pApprovalController->beginOutgoing(
		nGeneration, m_settings.nApprovalTimeoutSeconds * 1000 + kInitialApprovalResponseTimeoutMs);
	m_strLocalDeviceName = strDeviceName;
	const KSecurityStatus status = m_pSecurityController->beginOutgoing(
		request.strRequestId,
		nGeneration, strDeviceName,
		KPermissionScopes::fromInt(kAllPermissionScopeBits));
	if (status.isValid())
	{
		clearApproval(status.strProtocolReason);
		emit outgoingSecurityRejected(status);
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
	const KSecurityStatus status = m_pSecurityController->beginIncoming(
		strSourceAddress, nGeneration, strDeviceName);
	if (status.isValid())
	{
		emit incomingSecurityRejected(status);
	}
}

bool KAccessSessionFlow::handleTlsPairingMessage(
	const KTlsPairingMessage &message,
	quint64 nGeneration)
{
	return m_pSecurityController->handleMessage(message, nGeneration);
}

void KAccessSessionFlow::respondPairing(const QString &strRequestId,
	bool bAccepted,
	KPermissionScopes permissions)
{
	m_pSecurityController->respondPairing(strRequestId, bAccepted, permissions);
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
			m_pSecurityController->context();
		if (!m_pSecurityController->isAuthenticated()
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

void KAccessSessionFlow::handleAuthenticationRejected(const KSecurityStatus &status)
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	clearApproval(status.strProtocolReason);
	if (request.side == IncomingAccessApprovalSide)
		emit incomingSecurityRejected(status);
	else if (request.side == OutgoingAccessApprovalSide)
		emit outgoingSecurityRejected(status);
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
	if (m_pSecurityController->isActive())
		m_pSecurityController->cancel(strReason, bNotifyRemote, false);
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
	return m_pSecurityController->context();
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
