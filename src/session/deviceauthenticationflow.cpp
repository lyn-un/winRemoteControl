#include "session/deviceauthenticationflow.h"

#include "core/security/deviceidentityprovider.h"
#include "core/security/securitycanonicalwriter.h"
#include "core/security/trusteddevicestore.h"

#include <QtCore/QCryptographicHash>
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
	QObject *pParent)
	: QObject(pParent)
	, m_pIdentityProvider(pIdentityProvider)
	, m_pTrustedDeviceStore(pTrustedDeviceStore)
	, m_pTimer(new QTimer(this))
{
	Q_ASSERT(m_pIdentityProvider != nullptr);
	Q_ASSERT(m_pTrustedDeviceStore != nullptr);
	m_pTimer->setSingleShot(true);
	connect(m_pTimer, &QTimer::timeout, this,
		[this]() { fail(QStringLiteral("authentication_timeout"), true); });
}

void KDeviceAuthenticationFlow::setApprovalTimeoutSeconds(int nTimeoutSeconds)
{
	m_nApprovalTimeoutSeconds = qBound(10, nTimeoutSeconds, 120);
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
		|| !loadTrust(pErrorMessage))
	{
		return false;
	}
	m_bOutgoing = true;
	m_bActive = true;
	m_nGeneration = nGeneration;
	m_strLocalDeviceName = strDeviceName.left(128);
	m_context.strRequestId = strRequestId;
	m_context.requestedPermissions = requestedPermissions;
	m_localNonce = m_pIdentityProvider->randomBytes(32, pErrorMessage);
	if (m_localNonce.size() != 32)
	{
		clear(QStringLiteral("identity_unavailable"));
		return false;
	}
	KIdentityMessage hello;
	hello.type = HelloIdentityMessageType;
	hello.strRequestId = strRequestId;
	hello.strDeviceId = m_localIdentity.strDeviceId;
	hello.strDeviceName = m_strLocalDeviceName;
	hello.publicKey = m_localIdentity.publicKey;
	hello.nonce = m_localNonce;
	hello.permissions = requestedPermissions;
	m_pTimer->start(kInitialIdentityTimeoutMs);
	emit messageReady(hello);
	return true;
}

bool KDeviceAuthenticationFlow::beginIncoming(const QString &strSourceAddress,
	quint64 nGeneration,
	const QString &strDeviceName,
	QString *pErrorMessage)
{
	clear(QStringLiteral("new_authentication"));
	if (!loadTrust(pErrorMessage))
		return false;
	m_bOutgoing = false;
	m_bActive = true;
	m_nGeneration = nGeneration;
	m_strSourceAddress = strSourceAddress;
	m_strLocalDeviceName = strDeviceName.left(128);
	m_pTimer->start(kInitialIdentityTimeoutMs);
	return true;
}

bool KDeviceAuthenticationFlow::handleMessage(const KIdentityMessage &message,
	quint64 nGeneration)
{
	if (!m_bActive || nGeneration != m_nGeneration)
		return false;
	if (!m_context.strRequestId.isEmpty()
		&& message.strRequestId != m_context.strRequestId)
	{
		return false;
	}
	if (message.type == RejectedIdentityMessageType)
	{
		clear(message.strReason);
		emit authenticationRejected(message.strReason);
		return true;
	}
	if (message.type == HelloIdentityMessageType)
		return handleHello(message);
	if (message.type == ChallengeIdentityMessageType)
		return handleChallenge(message);
	if (message.type == ProofIdentityMessageType)
		return handleProof(message);
	if (message.type == PairingDecisionIdentityMessageType)
		return handlePairingDecision(message);
	if (message.type == AuthenticatedIdentityMessageType)
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
	emit pairingCleared(strRequestId,
		bAccepted ? QStringLiteral("accepted") : QStringLiteral("pairing_rejected"));
	if (!bAccepted)
	{
		recordSourceFailure();
		fail(QStringLiteral("pairing_rejected"), true);
		return;
	}
	KPermissionScopes permissions = m_bOutgoing
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
		KIdentityMessage rejected;
		rejected.type = RejectedIdentityMessageType;
		rejected.strRequestId = m_context.strRequestId;
		rejected.strReason = KIdentityMessageCodec::isValidRejectReason(strReason)
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

bool KDeviceAuthenticationFlow::handleHello(const KIdentityMessage &message)
{
	if (m_bOutgoing || !m_context.strRequestId.isEmpty())
		return false;
	if (isSourceRateLimited())
	{
		fail(QStringLiteral("pairing_rate_limited"), true);
		return true;
	}
	m_context.strRequestId = message.strRequestId;
	m_context.strRemoteDeviceId = message.strDeviceId;
	m_context.strRemoteDeviceName = message.strDeviceName;
	m_context.remotePublicKey = message.publicKey;
	m_context.strRemoteFingerprint = DevicePublicKeyFingerprint(message.publicKey);
	m_context.requestedPermissions = message.permissions;
	m_remoteNonce = message.nonce;
	QString strReason;
	if (!inspectPeerTrust(&strReason))
	{
		fail(strReason, true);
		return true;
	}
	QString strError;
	m_localNonce = m_pIdentityProvider->randomBytes(32, &strError);
	if (m_localNonce.size() != 32)
	{
		fail(QStringLiteral("identity_unavailable"), true);
		return true;
	}
	m_context.transcriptHash = QCryptographicHash::hash(
		transcriptData(), QCryptographicHash::Sha256);
	QByteArray signature;
	if (!m_pIdentityProvider->sign(proofData(QStringLiteral("controlled")),
		&signature, &strError))
	{
		fail(QStringLiteral("identity_unavailable"), true);
		return true;
	}
	KIdentityMessage challenge;
	challenge.type = ChallengeIdentityMessageType;
	challenge.strRequestId = m_context.strRequestId;
	challenge.strDeviceId = m_localIdentity.strDeviceId;
	challenge.strDeviceName = m_strLocalDeviceName;
	challenge.publicKey = m_localIdentity.publicKey;
	challenge.nonce = m_localNonce;
	challenge.signature = signature;
	m_pTimer->start(m_nApprovalTimeoutSeconds * 1000);
	emit messageReady(challenge);
	return true;
}

bool KDeviceAuthenticationFlow::handleChallenge(const KIdentityMessage &message)
{
	if (!m_bOutgoing || !m_context.strRemoteDeviceId.isEmpty())
		return false;
	m_context.strRemoteDeviceId = message.strDeviceId;
	m_context.strRemoteDeviceName = message.strDeviceName;
	m_context.remotePublicKey = message.publicKey;
	m_context.strRemoteFingerprint = DevicePublicKeyFingerprint(message.publicKey);
	m_remoteNonce = message.nonce;
	QString strReason;
	if (!inspectPeerTrust(&strReason))
	{
		fail(strReason, true);
		return true;
	}
	m_context.transcriptHash = QCryptographicHash::hash(
		transcriptData(), QCryptographicHash::Sha256);
	if (!m_pIdentityProvider->verify(message.publicKey,
		proofData(QStringLiteral("controlled")), message.signature, nullptr))
	{
		fail(QStringLiteral("signature_invalid"), true);
		return true;
	}
	m_bRemoteProofVerified = true;
	QByteArray signature;
	if (!m_pIdentityProvider->sign(proofData(QStringLiteral("controller")),
		&signature, nullptr))
	{
		fail(QStringLiteral("identity_unavailable"), true);
		return true;
	}
	KIdentityMessage proof;
	proof.type = ProofIdentityMessageType;
	proof.strRequestId = m_context.strRequestId;
	proof.strDeviceId = m_localIdentity.strDeviceId;
	proof.transcriptHash = m_context.transcriptHash;
	proof.signature = signature;
	m_pTimer->start(m_nApprovalTimeoutSeconds * 1000);
	emit messageReady(proof);
	if (!m_bActive)
		return true;
	beginPairingOrAutomaticDecision();
	return true;
}

bool KDeviceAuthenticationFlow::handleProof(const KIdentityMessage &message)
{
	if (m_bOutgoing || message.strDeviceId != m_context.strRemoteDeviceId
		|| message.transcriptHash != m_context.transcriptHash)
	{
		return false;
	}
	if (!m_pIdentityProvider->verify(m_context.remotePublicKey,
		proofData(QStringLiteral("controller")), message.signature, nullptr))
	{
		recordSourceFailure();
		fail(QStringLiteral("signature_invalid"), true);
		return true;
	}
	m_bRemoteProofVerified = true;
	beginPairingOrAutomaticDecision();
	return true;
}

bool KDeviceAuthenticationFlow::handlePairingDecision(
	const KIdentityMessage &message)
{
	if (!m_bRemoteProofVerified
		|| message.strDeviceId != m_context.strRemoteDeviceId
		|| message.transcriptHash != m_context.transcriptHash
		|| !m_pIdentityProvider->verify(m_context.remotePublicKey,
			decisionData(message.strDeviceId, message.bAccepted, message.permissions),
			message.signature, nullptr))
	{
		fail(QStringLiteral("signature_invalid"), true);
		return true;
	}
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
	const KIdentityMessage &message)
{
	const KPermissionScopes effective = m_bOutgoing
		? m_remoteDecisionPermissions : m_localDecisionPermissions;
	if (message.strDeviceId != m_context.strRemoteDeviceId
		|| message.transcriptHash != m_context.transcriptHash
		|| message.permissions != effective
		|| !m_pIdentityProvider->verify(m_context.remotePublicKey,
			authenticatedData(message.strDeviceId, message.permissions),
			message.signature, nullptr))
	{
		fail(QStringLiteral("signature_invalid"), true);
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
	m_localIdentity = m_pIdentityProvider->identity();
	QString strStoreError;
	m_trustedDevices = m_pTrustedDeviceStore->loadDevices(&strStoreError);
	if (!strStoreError.isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strStoreError;
		return false;
	}
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
	if (pTrusted->publicKey != m_context.remotePublicKey)
	{
		*pReason = QStringLiteral("device_key_changed");
		return false;
	}
	m_context.bTrustedDevice = true;
	m_context.bRequestWithinTrust =
		(m_context.requestedPermissions & ~pTrusted->permissionLimit).toInt() == 0;
	return true;
}

void KDeviceAuthenticationFlow::beginPairingOrAutomaticDecision()
{
	if (!m_bRemoteProofVerified || m_bLocalDecisionSent || m_bPairingPromptVisible)
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
	m_bPairingPromptVisible = true;
	const qint64 nExpiresAtMs = QDateTime::currentMSecsSinceEpoch()
		+ m_nApprovalTimeoutSeconds * 1000LL;
	emit pairingRequested(m_context.strRequestId,
		m_context.strRemoteDeviceName,
		m_context.strRemoteFingerprint,
		pairingCode(),
		m_context.requestedPermissions,
		nExpiresAtMs);
}

void KDeviceAuthenticationFlow::sendPairingDecision(bool bAccepted,
	KPermissionScopes permissions)
{
	KIdentityMessage decision;
	decision.type = PairingDecisionIdentityMessageType;
	decision.strRequestId = m_context.strRequestId;
	decision.strDeviceId = m_localIdentity.strDeviceId;
	decision.transcriptHash = m_context.transcriptHash;
	decision.bAccepted = bAccepted;
	decision.permissions = permissions;
	if (!m_pIdentityProvider->sign(decisionData(decision.strDeviceId,
		decision.bAccepted, decision.permissions), &decision.signature, nullptr))
	{
		fail(QStringLiteral("identity_unavailable"), true);
		return;
	}
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
	KIdentityMessage authenticated;
	authenticated.type = AuthenticatedIdentityMessageType;
	authenticated.strRequestId = m_context.strRequestId;
	authenticated.strDeviceId = m_localIdentity.strDeviceId;
	authenticated.transcriptHash = m_context.transcriptHash;
	authenticated.permissions = effective;
	if (!m_pIdentityProvider->sign(authenticatedData(authenticated.strDeviceId,
		effective), &authenticated.signature, nullptr))
	{
		fail(QStringLiteral("identity_unavailable"), true);
		return;
	}
	m_context.effectivePermissions = effective;
	m_bLocalAuthenticatedSent = true;
	emit messageReady(authenticated);
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
		trusted.publicKey = m_context.remotePublicKey;
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
		pTrusted->nLastAuthenticatedAtMs = nNowMs;
	}
	return m_pTrustedDeviceStore->saveDevices(m_trustedDevices, pErrorMessage);
}

QByteArray KDeviceAuthenticationFlow::transcriptData() const
{
	const KDeviceIdentity controllerIdentity = m_bOutgoing
		? m_localIdentity
		: KDeviceIdentity{ m_context.strRemoteDeviceId,
			QStringLiteral("ecdsa-p256-sha256"), m_context.remotePublicKey,
			m_context.strRemoteFingerprint };
	const KDeviceIdentity controlledIdentity = m_bOutgoing
		? KDeviceIdentity{ m_context.strRemoteDeviceId,
			QStringLiteral("ecdsa-p256-sha256"), m_context.remotePublicKey,
			m_context.strRemoteFingerprint }
		: m_localIdentity;
	KSecurityCanonicalWriter writer;
	writer.appendString(QStringLiteral("wrc.identity-transcript"));
	writer.appendUInt32(1);
	writer.appendString(m_context.strRequestId);
	writer.appendString(controllerIdentity.strDeviceId);
	writer.appendBytes(controllerIdentity.publicKey);
	writer.appendBytes(m_bOutgoing ? m_localNonce : m_remoteNonce);
	writer.appendString(controlledIdentity.strDeviceId);
	writer.appendBytes(controlledIdentity.publicKey);
	writer.appendBytes(m_bOutgoing ? m_remoteNonce : m_localNonce);
	writer.appendUInt32(static_cast<quint32>(m_context.requestedPermissions.toInt()));
	return writer.data();
}

QByteArray KDeviceAuthenticationFlow::proofData(const QString &strSenderRole) const
{
	KSecurityCanonicalWriter writer;
	writer.appendString(QStringLiteral("wrc.identity-proof"));
	writer.appendBytes(m_context.transcriptHash);
	writer.appendString(strSenderRole);
	return writer.data();
}

QByteArray KDeviceAuthenticationFlow::decisionData(
	const QString &strSenderDeviceId,
	bool bAccepted,
	KPermissionScopes permissions) const
{
	KSecurityCanonicalWriter writer;
	writer.appendString(QStringLiteral("wrc.pairing-decision"));
	writer.appendBytes(m_context.transcriptHash);
	writer.appendString(strSenderDeviceId);
	writer.appendBool(bAccepted);
	writer.appendUInt32(static_cast<quint32>(permissions.toInt()));
	return writer.data();
}

QByteArray KDeviceAuthenticationFlow::authenticatedData(
	const QString &strSenderDeviceId,
	KPermissionScopes permissions) const
{
	KSecurityCanonicalWriter writer;
	writer.appendString(QStringLiteral("wrc.identity-authenticated"));
	writer.appendBytes(m_context.transcriptHash);
	writer.appendString(strSenderDeviceId);
	writer.appendUInt32(static_cast<quint32>(permissions.toInt()));
	return writer.data();
}

QString KDeviceAuthenticationFlow::pairingCode() const
{
	if (m_context.transcriptHash.size() < 4)
		return QString();
	const auto *pBytes = reinterpret_cast<const unsigned char *>(
		m_context.transcriptHash.constData());
	const quint32 nValue = (static_cast<quint32>(pBytes[0]) << 24)
		| (static_cast<quint32>(pBytes[1]) << 16)
		| (static_cast<quint32>(pBytes[2]) << 8)
		| static_cast<quint32>(pBytes[3]);
	return QStringLiteral("%1").arg(nValue % 1000000, 6, 10, QLatin1Char('0'));
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
	const QString strRequestId = m_context.strRequestId;
	if (bNotifyRemote && !strRequestId.isEmpty())
	{
		KIdentityMessage rejected;
		rejected.type = RejectedIdentityMessageType;
		rejected.strRequestId = strRequestId;
		rejected.strReason = KIdentityMessageCodec::isValidRejectReason(strReason)
			? strReason : QStringLiteral("cancelled");
		emit messageReady(rejected);
	}
	clear(strReason);
	emit authenticationRejected(strReason);
}

void KDeviceAuthenticationFlow::clear(const QString &strReason)
{
	const QString strRequestId = m_context.strRequestId;
	m_pTimer->stop();
	if (m_bPairingPromptVisible)
		emit pairingCleared(strRequestId, strReason);
	m_context = KDeviceAuthenticationContext();
	m_localIdentity = KDeviceIdentity();
	m_strLocalDeviceName.clear();
	m_strSourceAddress.clear();
	m_localNonce.clear();
	m_remoteNonce.clear();
	m_localDecisionPermissions = KPermissionScopes();
	m_remoteDecisionPermissions = KPermissionScopes();
	m_nGeneration = 0;
	m_bOutgoing = false;
	m_bActive = false;
	m_bAuthenticated = false;
	m_bRemoteProofVerified = false;
	m_bPairingPromptVisible = false;
	m_bLocalDecisionSent = false;
	m_bRemoteDecisionReceived = false;
	m_bLocalAuthenticatedSent = false;
	m_bRemoteAuthenticatedReceived = false;
}
