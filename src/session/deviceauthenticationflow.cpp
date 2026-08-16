#include "session/deviceauthenticationflow.h"

#include "common/sessiontracelogger.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/tlspairingverification.h"
#include "core/security/trusteddevicestore.h"
#include "core/transport/keyingmaterialexporter.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

namespace
{
	constexpr int kInitialIdentityTimeoutMs = 3000;
	constexpr int kMaximumSourceFailures = 5;
	constexpr qint64 kSourceFailureWindowMs = 10 * 60 * 1000LL;
}

KDeviceAuthenticationFlow::KDeviceAuthenticationFlow(
	KDeviceIdentityProvider *pIdentityProvider,
	KTrustedDeviceStore *pTrustedDeviceStore,
	KKeyingMaterialExporter *pKeyingMaterialExporter,
	QObject *pParent)
	: QObject(pParent)
	, m_pIdentityProvider(pIdentityProvider)
	, m_pTrustedDeviceStore(pTrustedDeviceStore)
	, m_pKeyingMaterialExporter(pKeyingMaterialExporter)
	, m_pTimer(new QTimer(this))
{
	Q_ASSERT(m_pIdentityProvider != nullptr);
	Q_ASSERT(m_pTrustedDeviceStore != nullptr);
	Q_ASSERT(m_pKeyingMaterialExporter != nullptr);
	m_pTimer->setSingleShot(true);
	connect(m_pTimer, &QTimer::timeout, this,
		[this]() { fail(QStringLiteral("authentication_timeout"), true); });
}

void KDeviceAuthenticationFlow::setApprovalTimeoutSeconds(int nTimeoutSeconds)
{
	m_nApprovalTimeoutSeconds = qBound(10, nTimeoutSeconds, 120);
}

void KDeviceAuthenticationFlow::setSecurePeerIdentity(const KTlsPeerIdentity &peer)
{
	m_securePeer = peer;
}

bool KDeviceAuthenticationFlow::beginOutgoing(const QString &strRequestId,
	quint64 nGeneration,
	const QString &strDeviceName,
	KPermissionScopes requestedPermissions,
	QString *pErrorMessage)
{
	clear(QStringLiteral("new_authentication"));
	if (QUuid(strRequestId).isNull()
		|| !requestedPermissions.testFlag(ViewScreenPermissionScope)
		|| !loadTrust(pErrorMessage) || !initializePeerContext(pErrorMessage))
	{
		return false;
	}
	m_bOutgoing = true;
	m_bActive = true;
	m_nGeneration = nGeneration;
	m_strLocalDeviceName = strDeviceName.left(128);
	m_context.strRequestId = strRequestId;
	m_context.requestedPermissions = requestedPermissions;
	m_pTimer->start(kInitialIdentityTimeoutMs);
	sendHello();
	return true;
}

bool KDeviceAuthenticationFlow::beginIncoming(const QString &strSourceAddress,
	quint64 nGeneration,
	const QString &strDeviceName,
	QString *pErrorMessage)
{
	clear(QStringLiteral("new_authentication"));
	if (!loadTrust(pErrorMessage) || !initializePeerContext(pErrorMessage))
		return false;
	m_strSourceAddress = strSourceAddress;
	if (isSourceRateLimited())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("pairing_rate_limited");
		return false;
	}
	m_bOutgoing = false;
	m_bActive = true;
	m_nGeneration = nGeneration;
	m_strLocalDeviceName = strDeviceName.left(128);
	m_pTimer->start(kInitialIdentityTimeoutMs);
	return true;
}

bool KDeviceAuthenticationFlow::handleMessage(const KTlsPairingMessage &message,
	quint64 nGeneration)
{
	if (!m_bActive || nGeneration != m_nGeneration)
		return false;
	if (!m_context.strRequestId.isEmpty()
		&& message.strRequestId != m_context.strRequestId)
	{
		return false;
	}
	if (message.type == RejectedTlsPairingMessageType)
	{
		clear(message.strReason);
		emit authenticationRejected(message.strReason);
		return true;
	}
	if (message.type == HelloTlsPairingMessageType)
		return handleHello(message);
	if (message.type == DecisionTlsPairingMessageType)
		return handlePairingDecision(message);
	if (message.type == ReadyTlsPairingMessageType)
		return handleAuthenticated(message);
	return false;
}

void KDeviceAuthenticationFlow::respondPairing(const QString &strRequestId,
	bool bAccepted,
	KPermissionScopes grantedPermissions)
{
	if (!m_bActive || !m_bPairingPromptVisible
		|| strRequestId != m_context.strRequestId)
	{
		return;
	}
	m_bPairingPromptVisible = false;
	KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
			: QStringLiteral("controlled"),
		QStringLiteral("pairing"),
		bAccepted ? QStringLiteral("accepted") : QStringLiteral("rejected"), -1,
		QStringLiteral("requestId=%1").arg(strRequestId));
	emit pairingCleared(strRequestId,
		bAccepted ? QStringLiteral("accepted") : QStringLiteral("pairing_rejected"));
	if (!bAccepted)
	{
		recordSourceFailure();
		fail(QStringLiteral("pairing_rejected"), true);
		return;
	}
	const KPermissionScopes permissions = m_bOutgoing
		? m_context.requestedPermissions
		: (grantedPermissions & m_context.requestedPermissions);
	if (!permissions.testFlag(ViewScreenPermissionScope))
	{
		fail(QStringLiteral("pairing_rejected"), true);
		return;
	}
	sendPairingDecision(true, permissions);
}

void KDeviceAuthenticationFlow::cancel(const QString &strReason,
	bool bNotifyRemote,
	bool bEmitOutcome)
{
	if (!m_bActive)
		return;
	if (bEmitOutcome)
	{
		fail(strReason, bNotifyRemote);
		return;
	}
	if (bNotifyRemote && !m_context.strRequestId.isEmpty())
	{
		KTlsPairingMessage rejected;
		rejected.type = RejectedTlsPairingMessageType;
		rejected.strRequestId = m_context.strRequestId;
		rejected.strReason = KTlsPairingMessageCodec::isValidRejectReason(strReason)
			? strReason : QStringLiteral("cancelled");
		emit messageReady(rejected);
	}
	clear(strReason);
}

bool KDeviceAuthenticationFlow::isActive() const
{
	return m_bActive;
}

bool KDeviceAuthenticationFlow::isAuthenticated() const
{
	return m_bAuthenticated;
}

KDeviceAuthenticationContext KDeviceAuthenticationFlow::context() const
{
	return m_context;
}

bool KDeviceAuthenticationFlow::handleHello(const KTlsPairingMessage &message)
{
	if (message.strVerificationMethod
		!= KTlsPairingVerification::verificationMethod())
	{
		fail(QStringLiteral("protocol_incompatible"), true);
		return true;
	}
	if (m_bHelloReceived || message.strDeviceId != m_securePeer.strDeviceId)
	{
		fail(QStringLiteral("device_key_changed"), true);
		return true;
	}
	if (!m_bOutgoing)
	{
		m_context.strRequestId = message.strRequestId;
		m_context.requestedPermissions = message.permissions;
	}
	else if (message.strRequestId != m_context.strRequestId)
	{
		return false;
	}
	m_context.strRemoteDeviceName = message.strDeviceName;
	m_bHelloReceived = true;
	QString strReason;
	if (!inspectPeerTrust(&strReason))
	{
		fail(strReason, true);
		return true;
	}
	if (!m_bHelloSent)
		sendHello();
	m_pTimer->start(m_nApprovalTimeoutSeconds * 1000);
	beginPairingOrAutomaticDecision();
	return true;
}

bool KDeviceAuthenticationFlow::handlePairingDecision(
	const KTlsPairingMessage &message)
{
	if (!m_bHelloReceived || message.strDeviceId != m_securePeer.strDeviceId)
		return false;
	if (!message.bAccepted)
	{
		clear(QStringLiteral("pairing_rejected"));
		emit authenticationRejected(QStringLiteral("pairing_rejected"));
		return true;
	}
	m_remoteDecisionPermissions = message.permissions;
	m_bRemoteDecisionReceived = true;
	trySendAuthenticated();
	return true;
}

bool KDeviceAuthenticationFlow::handleAuthenticated(
	const KTlsPairingMessage &message)
{
	const KPermissionScopes effective = m_bOutgoing
		? m_remoteDecisionPermissions : m_localDecisionPermissions;
	if (message.strDeviceId != m_securePeer.strDeviceId
		|| message.permissions != effective)
	{
		fail(QStringLiteral("protocol_incompatible"), true);
		return true;
	}
	m_bRemoteAuthenticatedReceived = true;
	trySendAuthenticated();
	tryComplete();
	return true;
}

bool KDeviceAuthenticationFlow::loadTrust(QString *pErrorMessage)
{
	if (!m_pIdentityProvider->initialize(pErrorMessage))
		return false;
	m_localCertificate = m_pIdentityProvider->certificate();
	QString strStoreError;
	m_trustedDevices = m_pTrustedDeviceStore->loadDevices(&strStoreError);
	if (!strStoreError.isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strStoreError;
		return false;
	}
	return m_localCertificate.isValid();
}

bool KDeviceAuthenticationFlow::initializePeerContext(QString *pErrorMessage)
{
	if (!m_securePeer.isValid())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("TLS peer identity is unavailable");
		return false;
	}
	m_context.strRemoteDeviceId = m_securePeer.strDeviceId;
	m_context.strRemoteFingerprint = m_securePeer.spkiFingerprint();
	return true;
}

bool KDeviceAuthenticationFlow::inspectPeerTrust(QString *pReason)
{
	KTrustedDevice *pTrusted = findPeerTrust();
	if (pTrusted == nullptr)
	{
		m_context.bTrustedDevice = false;
		m_context.bRequestWithinTrust = false;
		return true;
	}
	if (pTrusted->bRevoked)
	{
		*pReason = QStringLiteral("device_revoked");
		return false;
	}
	if (pTrusted->spkiSha256 != m_securePeer.spkiSha256)
	{
		*pReason = QStringLiteral("device_key_changed");
		return false;
	}
	m_context.bTrustedDevice = true;
	m_context.bRequestWithinTrust =
		(m_context.requestedPermissions & ~pTrusted->permissionLimit).toInt() == 0;
	return true;
}

void KDeviceAuthenticationFlow::sendHello()
{
	KTlsPairingMessage hello;
	hello.type = HelloTlsPairingMessageType;
	hello.strRequestId = m_context.strRequestId;
	hello.strDeviceId = m_localCertificate.strDeviceId;
	hello.strDeviceName = m_strLocalDeviceName;
	hello.strVerificationMethod = KTlsPairingVerification::verificationMethod();
	hello.permissions = m_context.requestedPermissions;
	m_bHelloSent = true;
	emit messageReady(hello);
}

void KDeviceAuthenticationFlow::beginPairingOrAutomaticDecision()
{
	if (!m_bHelloReceived || m_bLocalDecisionSent || m_bPairingPromptVisible)
		return;
	const KTrustedDevice *pTrusted = findPeerTrust();
	if (pTrusted != nullptr)
	{
		const KPermissionScopes permissions = m_bOutgoing
			? m_context.requestedPermissions
			: (pTrusted->permissionLimit & m_context.requestedPermissions);
		sendPairingDecision(true, permissions);
		return;
	}
	QString strError;
	if (!createPairingVerification(&strError))
	{
		fail(QStringLiteral("channel_binding_unavailable"), true);
		return;
	}
	m_bPairingPromptVisible = true;
	const qint64 nExpiresAtMs = QDateTime::currentMSecsSinceEpoch()
		+ m_nApprovalTimeoutSeconds * 1000LL;
	emit pairingRequested(m_context.strRequestId,
		m_context.strRemoteDeviceName,
		m_bOutgoing ? QStringLiteral("controller") : QStringLiteral("controlled"),
		m_strVerificationCode,
		controllerFingerprint(), controlledFingerprint(),
		m_securePeer.strTlsProtocol, m_securePeer.strCipherSuite,
		m_context.requestedPermissions, nExpiresAtMs);
}

bool KDeviceAuthenticationFlow::createPairingVerification(QString *pErrorMessage)
{
	const QString strControllerDeviceId = m_bOutgoing
		? m_localCertificate.strDeviceId : m_context.strRemoteDeviceId;
	const QString strControlledDeviceId = m_bOutgoing
		? m_context.strRemoteDeviceId : m_localCertificate.strDeviceId;
	const QByteArray controllerSpkiSha256 = m_bOutgoing
		? m_localCertificate.spkiSha256 : m_securePeer.spkiSha256;
	const QByteArray controlledSpkiSha256 = m_bOutgoing
		? m_securePeer.spkiSha256 : m_localCertificate.spkiSha256;
	const QByteArray context = KTlsPairingVerification::createContext(
		m_context.strRequestId, strControllerDeviceId, strControlledDeviceId,
		controllerSpkiSha256, controlledSpkiSha256, pErrorMessage);
	if (context.isEmpty())
		return false;

	QByteArray keyingMaterial;
	if (!m_pKeyingMaterialExporter->exportKeyingMaterial(
		KTlsPairingVerification::exporterLabel(), context,
		KTlsPairingVerification::kKeyingMaterialBytes,
		&keyingMaterial, pErrorMessage))
	{
		return false;
	}
	m_strVerificationCode = KTlsPairingVerification::numericCode(
		keyingMaterial, pErrorMessage);
	keyingMaterial.fill('\0');
	return !m_strVerificationCode.isEmpty();
}

void KDeviceAuthenticationFlow::sendPairingDecision(bool bAccepted,
	KPermissionScopes permissions)
{
	KTlsPairingMessage decision;
	decision.type = DecisionTlsPairingMessageType;
	decision.strRequestId = m_context.strRequestId;
	decision.strDeviceId = m_localCertificate.strDeviceId;
	decision.bAccepted = bAccepted;
	decision.permissions = permissions;
	m_localDecisionPermissions = permissions;
	m_bLocalDecisionSent = true;
	emit messageReady(decision);
	trySendAuthenticated();
}

void KDeviceAuthenticationFlow::trySendAuthenticated()
{
	if (!m_bLocalDecisionSent || !m_bRemoteDecisionReceived
		|| m_bLocalAuthenticatedSent)
	{
		return;
	}
	const KPermissionScopes effective = m_bOutgoing
		? m_remoteDecisionPermissions : m_localDecisionPermissions;
	if (!effective.testFlag(ViewScreenPermissionScope))
	{
		fail(QStringLiteral("pairing_rejected"), true);
		return;
	}
	KTlsPairingMessage ready;
	ready.type = ReadyTlsPairingMessageType;
	ready.strRequestId = m_context.strRequestId;
	ready.strDeviceId = m_localCertificate.strDeviceId;
	ready.permissions = effective;
	m_context.effectivePermissions = effective;
	m_bLocalAuthenticatedSent = true;
	emit messageReady(ready);
	tryComplete();
}

void KDeviceAuthenticationFlow::tryComplete()
{
	if (!m_bLocalAuthenticatedSent || !m_bRemoteAuthenticatedReceived
		|| m_bAuthenticated)
	{
		return;
	}
	QString strError;
	if (!persistPeerTrust(&strError))
	{
		fail(QStringLiteral("identity_unavailable"), true);
		return;
	}
	m_bAuthenticated = true;
	m_bActive = false;
	m_pTimer->stop();
	if (m_bPairingPromptVisible)
	{
		m_bPairingPromptVisible = false;
		emit pairingCleared(m_context.strRequestId, QStringLiteral("authenticated"));
	}
	KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
			: QStringLiteral("controlled"),
		QStringLiteral("authentication"), QStringLiteral("authenticated"), -1,
		QStringLiteral("deviceId=%1 fingerprint=%2 trusted=%3 permissions=%4")
			.arg(m_context.strRemoteDeviceId,
				QString::fromLatin1(m_securePeer.spkiSha256.toHex().left(12)))
			.arg(m_context.bTrustedDevice ? 1 : 0)
			.arg(m_context.effectivePermissions.toInt()));
	emit authenticationSucceeded(m_context);
}

bool KDeviceAuthenticationFlow::persistPeerTrust(QString *pErrorMessage)
{
	KTrustedDevice *pTrusted = findPeerTrust();
	const qint64 nNowMs = QDateTime::currentMSecsSinceEpoch();
	if (pTrusted == nullptr)
	{
		KTrustedDevice trusted;
		trusted.strDeviceId = m_context.strRemoteDeviceId;
		trusted.spkiSha256 = m_securePeer.spkiSha256;
		trusted.certificateSha256 = m_securePeer.certificateSha256;
		trusted.strFingerprint = m_context.strRemoteFingerprint;
		trusted.strAlias = m_context.strRemoteDeviceName;
		trusted.strAdvertisedName = m_context.strRemoteDeviceName;
		trusted.permissionLimit = m_context.effectivePermissions;
		trusted.nPairedAtMs = nNowMs;
		trusted.nLastAuthenticatedAtMs = nNowMs;
		m_trustedDevices.append(trusted);
	}
	else
	{
		pTrusted->strAdvertisedName = m_context.strRemoteDeviceName;
		pTrusted->certificateSha256 = m_securePeer.certificateSha256;
		pTrusted->nLastAuthenticatedAtMs = nNowMs;
	}
	return m_pTrustedDeviceStore->saveDevices(m_trustedDevices, pErrorMessage);
}

QString KDeviceAuthenticationFlow::localFingerprint() const
{
	return m_localCertificate.spkiFingerprint();
}

QString KDeviceAuthenticationFlow::controllerFingerprint() const
{
	return m_bOutgoing ? localFingerprint() : m_context.strRemoteFingerprint;
}

QString KDeviceAuthenticationFlow::controlledFingerprint() const
{
	return m_bOutgoing ? m_context.strRemoteFingerprint : localFingerprint();
}

KTrustedDevice *KDeviceAuthenticationFlow::findPeerTrust()
{
	for (KTrustedDevice &device : m_trustedDevices)
	{
		if (device.strDeviceId == m_context.strRemoteDeviceId)
			return &device;
	}
	return nullptr;
}

const KTrustedDevice *KDeviceAuthenticationFlow::findPeerTrust() const
{
	for (const KTrustedDevice &device : m_trustedDevices)
	{
		if (device.strDeviceId == m_context.strRemoteDeviceId)
			return &device;
	}
	return nullptr;
}

bool KDeviceAuthenticationFlow::isSourceRateLimited()
{
	QVector<qint64> &failures = m_sourceFailures[m_strSourceAddress];
	const qint64 nCutoff = QDateTime::currentMSecsSinceEpoch() - kSourceFailureWindowMs;
	while (!failures.isEmpty() && failures.first() < nCutoff)
		failures.removeFirst();
	return failures.size() >= kMaximumSourceFailures;
}

void KDeviceAuthenticationFlow::recordSourceFailure()
{
	if (!m_bOutgoing && !m_strSourceAddress.isEmpty())
		m_sourceFailures[m_strSourceAddress].append(QDateTime::currentMSecsSinceEpoch());
}

void KDeviceAuthenticationFlow::fail(const QString &strReason,
	bool bNotifyRemote)
{
	KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
			: QStringLiteral("controlled"),
		QStringLiteral("authentication"), QStringLiteral("rejected"), -1,
		QStringLiteral("reason=%1 deviceId=%2 fingerprint=%3")
			.arg(strReason, m_context.strRemoteDeviceId,
				QString::fromLatin1(m_securePeer.spkiSha256.toHex().left(12))));
	const QString strRequestId = m_context.strRequestId;
	if (bNotifyRemote && !strRequestId.isEmpty())
	{
		KTlsPairingMessage rejected;
		rejected.type = RejectedTlsPairingMessageType;
		rejected.strRequestId = strRequestId;
		rejected.strReason = KTlsPairingMessageCodec::isValidRejectReason(strReason)
			? strReason : QStringLiteral("cancelled");
		emit messageReady(rejected);
	}
	clear(strReason);
	emit authenticationRejected(strReason);
}

void KDeviceAuthenticationFlow::clear(const QString &strReason,
	bool bKeepPeerIdentity)
{
	const QString strRequestId = m_context.strRequestId;
	m_pTimer->stop();
	if (m_bPairingPromptVisible)
		emit pairingCleared(strRequestId, strReason);
	m_context = KDeviceAuthenticationContext();
	m_localCertificate = KDeviceCertificate();
	if (!bKeepPeerIdentity)
		m_securePeer = KTlsPeerIdentity();
	m_strLocalDeviceName.clear();
	m_strSourceAddress.clear();
	m_strVerificationCode.clear();
	m_localDecisionPermissions = KPermissionScopes();
	m_remoteDecisionPermissions = KPermissionScopes();
	m_nGeneration = 0;
	m_bOutgoing = false;
	m_bActive = false;
	m_bAuthenticated = false;
	m_bHelloSent = false;
	m_bHelloReceived = false;
	m_bPairingPromptVisible = false;
	m_bLocalDecisionSent = false;
	m_bRemoteDecisionReceived = false;
	m_bLocalAuthenticatedSent = false;
	m_bRemoteAuthenticatedReceived = false;
}
