#include "session/securitysessioncontroller.h"

#include "core/transport/signalingtransport.h"

KSecuritySessionController::KSecuritySessionController(
	KSignalingTransport *pTransport,
	KDeviceIdentityProvider *pIdentityProvider,
	KTrustedDeviceStore *pTrustedDeviceStore,
	QObject *pParent)
	: QObject(pParent)
	, m_pTransport(pTransport)
	, m_pAuthenticationFlow(new KDeviceAuthenticationFlow(
		pIdentityProvider, pTrustedDeviceStore, pTransport, this))
{
	Q_ASSERT(m_pTransport != nullptr);
	m_pAuthenticationFlow->setPairingCommand(&m_pairingCommand);
	connect(m_pTransport, &KSignalingTransport::secureChannelEstablished,
		m_pAuthenticationFlow, &KDeviceAuthenticationFlow::setSecurePeerIdentity);
	connect(m_pTransport, &KSignalingTransport::connectionLost,
		this, [this]()
		{
			m_pAuthenticationFlow->cancel(
				QStringLiteral("connection_lost"), false, false);
			m_pAuthenticationFlow->setSecurePeerIdentity(KTlsPeerIdentity());
		});
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::messageReady,
		this, [this](const KTlsPairingMessage &message)
		{ m_pTransport->sendMessage(KTlsPairingMessageCodec::encode(message)); });
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::pairingRequested,
		this, &KSecuritySessionController::pairingRequested);
	connect(m_pAuthenticationFlow, &KDeviceAuthenticationFlow::pairingCleared,
		this, &KSecuritySessionController::pairingCleared);
	connect(m_pAuthenticationFlow,
		&KDeviceAuthenticationFlow::authenticationSucceeded,
		this, &KSecuritySessionController::authenticationSucceeded);
	connect(m_pAuthenticationFlow,
		&KDeviceAuthenticationFlow::authenticationRejected,
		this, &KSecuritySessionController::authenticationRejected);
}

void KSecuritySessionController::setAdmissionController(
	KAdmissionController *pController)
{
	m_pAuthenticationFlow->setAdmissionController(pController);
}

void KSecuritySessionController::setApprovalTimeoutSeconds(int nTimeoutSeconds)
{
	m_pAuthenticationFlow->setApprovalTimeoutSeconds(nTimeoutSeconds);
}

KSecurityStatus KSecuritySessionController::beginOutgoing(
	const QString &strRequestId,
	quint64 nGeneration,
	const QString &strDeviceName,
	KPermissionScopes requestedPermissions)
{
	return m_pAuthenticationFlow->beginOutgoing(strRequestId, nGeneration,
		strDeviceName, requestedPermissions);
}

KSecurityStatus KSecuritySessionController::beginIncoming(
	const QString &strSourceAddress,
	quint64 nGeneration,
	const QString &strDeviceName)
{
	return m_pAuthenticationFlow->beginIncoming(strSourceAddress, nGeneration,
		strDeviceName);
}

bool KSecuritySessionController::handleMessage(
	const KTlsPairingMessage &message,
	quint64 nGeneration)
{
	return m_pAuthenticationFlow->handleMessage(message, nGeneration);
}

void KSecuritySessionController::respondPairing(const QString &strRequestId,
	bool bAccepted,
	KPermissionScopes permissions)
{
	m_pAuthenticationFlow->respondPairing(strRequestId, bAccepted, permissions);
}

void KSecuritySessionController::cancel(const QString &strReason,
	bool bNotifyRemote,
	bool bEmitOutcome)
{
	m_pAuthenticationFlow->cancel(strReason, bNotifyRemote, bEmitOutcome);
}

void KSecuritySessionController::clearPeerIdentity()
{
	m_pAuthenticationFlow->setSecurePeerIdentity(KTlsPeerIdentity());
}

bool KSecuritySessionController::isActive() const
{
	return m_pAuthenticationFlow->isActive();
}

bool KSecuritySessionController::isAuthenticated() const
{
	return m_pAuthenticationFlow->isAuthenticated();
}

KDeviceAuthenticationContext KSecuritySessionController::context() const
{
	return m_pAuthenticationFlow->context();
}
