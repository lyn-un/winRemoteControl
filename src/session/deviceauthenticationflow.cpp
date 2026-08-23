#include "session/deviceauthenticationflow.h"

#include "common/sessiontracelogger.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/admissioncontroller.h"
#include "core/security/tlspairingverification.h"
#include "core/security/trusteddevicestore.h"
#include "core/transport/keyingmaterialexporter.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

namespace
{
	constexpr int kInitialIdentityTimeoutMs = 3000;

	bool ShouldRecordSourceFailure(const QString &strReason)
	{
		return strReason == QStringLiteral("authentication_timeout")
			|| strReason == QStringLiteral("protocol_incompatible")
			|| strReason == QStringLiteral("pairing_rejected")
			|| strReason == QStringLiteral("device_key_changed")
			|| strReason == QStringLiteral("device_revoked");
	}
}

KDeviceAuthenticationFlow::KDeviceAuthenticationFlow(
	KDeviceIdentityProvider *pIdentityProvider,
	KTrustedDeviceStore *pTrustedDeviceStore,
	KKeyingMaterialExporter *pKeyingMaterialExporter,
	QObject *pParent)
	: QObject(pParent)
	, m_pIdentityProvider(pIdentityProvider)
	, m_pKeyingMaterialExporter(pKeyingMaterialExporter)
	, m_trustedDeviceService(pTrustedDeviceStore)
	, m_pTimer(new QTimer(this))
{
	Q_ASSERT(m_pIdentityProvider != nullptr);
	Q_ASSERT(pTrustedDeviceStore != nullptr);
	Q_ASSERT(m_pKeyingMaterialExporter != nullptr);
	m_pFallbackAdmissionController = new KAdmissionController();
	m_pAdmissionController = m_pFallbackAdmissionController;
	m_pTimer->setSingleShot(true);
	connect(m_pTimer, &QTimer::timeout, this,
		[this]() { fail(QStringLiteral("authentication_timeout"), true,
			UserApprovalSecurityStage); });
}

KDeviceAuthenticationFlow::~KDeviceAuthenticationFlow()
{
	delete m_pFallbackAdmissionController;
}

void KDeviceAuthenticationFlow::setAdmissionController(
	KAdmissionController *pController)
{
	m_pAdmissionController = pController != nullptr
		? pController : m_pFallbackAdmissionController;
}

void KDeviceAuthenticationFlow::setPairingCommand(KPairingCommand *pCommand)
{
	if (m_bActive)
		return;
	m_pPairingCommand = pCommand != nullptr
		? pCommand : &m_fallbackPairingCommand;
}

void KDeviceAuthenticationFlow::setApprovalTimeoutSeconds(int nTimeoutSeconds)
{
	m_nApprovalTimeoutSeconds = qBound(10, nTimeoutSeconds, 120);
}

void KDeviceAuthenticationFlow::setSecurePeerIdentity(const KTlsPeerIdentity &peer)
{
	m_securePeer = peer;
}

KSecurityStatus KDeviceAuthenticationFlow::beginOutgoing(
	const QString &strRequestId,
	quint64 nGeneration,
	const QString &strDeviceName,
	KPermissionScopes requestedPermissions)
{
	clear(QStringLiteral("new_authentication"));
	if (QUuid(strRequestId).isNull()
		|| !requestedPermissions.testFlag(ViewScreenPermissionScope))
	{
		return entryFailure(QStringLiteral("protocol_incompatible"),
			PairingHelloSecurityStage, QStringLiteral("Invalid authentication request"),
			strRequestId, nGeneration, true);
	}
	QString strError;
	if (!loadTrust(&strError))
	{
		const QString strReason = m_trustedDeviceService.lastLoadError()
			== TamperedTrustedDeviceStoreError
			? QStringLiteral("trust_store_tampered")
			: QStringLiteral("identity_unavailable");
		return entryFailure(strReason, TrustLoadSecurityStage, strError,
			strRequestId, nGeneration, true);
	}
	if (!initializePeerContext(&strError))
	{
		return entryFailure(QStringLiteral("certificate_invalid"),
			TlsHandshakeSecurityStage, strError, strRequestId, nGeneration, true);
	}
	m_bOutgoing = true;
	m_bActive = true;
	m_nGeneration = nGeneration;
	m_strLocalDeviceName = strDeviceName.left(128);
	m_context.strRequestId = strRequestId;
	m_context.requestedPermissions = requestedPermissions;
	m_pPairingCommand->begin(strRequestId, nGeneration);
	m_pTimer->start(kInitialIdentityTimeoutMs);
	sendHello();
	return KSecurityStatus();
}

KSecurityStatus KDeviceAuthenticationFlow::beginIncoming(
	const QString &strSourceAddress,
	quint64 nGeneration,
	const QString &strDeviceName)
{
	clear(QStringLiteral("new_authentication"));
	m_strSourceAddress = KAdmissionController::normalizedSourceAddress(
		strSourceAddress);
	if (isSourceRateLimited())
	{
		return entryFailure(QStringLiteral("pairing_rate_limited"),
			PairingHelloSecurityStage, QString(), QString(), nGeneration, false);
	}
	QString strError;
	if (!loadTrust(&strError))
	{
		const QString strReason = m_trustedDeviceService.lastLoadError()
			== TamperedTrustedDeviceStoreError
			? QStringLiteral("trust_store_tampered")
			: QStringLiteral("identity_unavailable");
		return entryFailure(strReason, TrustLoadSecurityStage, strError,
			QString(), nGeneration, false);
	}
	if (!initializePeerContext(&strError))
	{
		return entryFailure(QStringLiteral("certificate_invalid"),
			TlsHandshakeSecurityStage, strError, QString(), nGeneration, false);
	}
	m_bOutgoing = false;
	m_bActive = true;
	m_nGeneration = nGeneration;
	m_strLocalDeviceName = strDeviceName.left(128);
	m_pTimer->start(kInitialIdentityTimeoutMs);
	return KSecurityStatus();
}

KSecurityStatus KDeviceAuthenticationFlow::entryFailure(
	const QString &strReason,
	KSecurityStage stage,
	const QString &strTechnicalMessage,
	const QString &strRequestId,
	quint64 nGeneration,
	bool bOutgoing) const
{
	KSecurityStatus status = KSecurityStatus::fromProtocolReason(
		strReason, stage, strTechnicalMessage);
	status.strRequestId = strRequestId;
	status.nGeneration = nGeneration;
	status.bOutgoing = bOutgoing;
	return status;
}

bool KDeviceAuthenticationFlow::handleMessage(const KTlsPairingMessage &message,
	quint64 nGeneration)
{
	if (!m_bActive || nGeneration != m_nGeneration)
		return false;
	if (message.type != HelloTlsPairingMessageType
		&& !transaction().matches(message.strRequestId, nGeneration))
	{
		KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
				: QStringLiteral("controlled"),
			QStringLiteral("pairing_message_dropped"), QStringLiteral("stale"), -1,
			QStringLiteral("requestId=%1 generation=%2 type=%3")
				.arg(message.strRequestId).arg(nGeneration)
				.arg(KTlsPairingMessageCodec::typeName(message.type)));
		return false;
	}
	if (message.type == RejectedTlsPairingMessageType)
	{
		fail(message.strReason, false, UserApprovalSecurityStage);
		return true;
	}
	if (message.type == HelloTlsPairingMessageType)
		return handleHello(message);
	if (message.type == DecisionTlsPairingMessageType)
		return handlePairingDecision(message);
	if (message.type == ReadyTlsPairingMessageType)
		return handlePrepared(message);
	if (message.type == CommittedTlsPairingMessageType)
		return handleCommitted(message);
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
		fail(QStringLiteral("pairing_rejected"), true,
			UserApprovalSecurityStage);
		return;
	}
	const KPermissionScopes permissions = m_bOutgoing
		? m_context.requestedPermissions
		: (grantedPermissions & m_context.requestedPermissions);
	if (!permissions.testFlag(ViewScreenPermissionScope))
	{
		fail(QStringLiteral("pairing_rejected"), true,
			UserApprovalSecurityStage);
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
	rollbackPairing(strReason);
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
		sendPairingMessage(rejected);
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
	const KPairingHelloDisposition disposition =
		m_pPairingCommand->classifyHello(message, m_nGeneration);
	if (disposition == ConflictingRequestPairingHelloDisposition)
	{
		KSessionTraceLogger::write(m_bOutgoing
				? QStringLiteral("controller") : QStringLiteral("controlled"),
			QStringLiteral("pairing_hello"), QStringLiteral("conflict"), -1,
			QStringLiteral("activeRequestId=%1 conflictingRequestId=%2 generation=%3")
				.arg(transaction().requestId(), message.strRequestId)
				.arg(m_nGeneration));
		KTlsPairingMessage rejected;
		rejected.type = RejectedTlsPairingMessageType;
		rejected.strRequestId = message.strRequestId;
		rejected.strReason = QStringLiteral("protocol_incompatible");
		sendPairingMessage(rejected, false);
		return true;
	}
	if (disposition == ConflictingPayloadPairingHelloDisposition)
	{
		fail(QStringLiteral("protocol_incompatible"), true,
			PairingHelloSecurityStage,
			QStringLiteral("Conflicting duplicate pairing Hello"));
		return true;
	}
	if (disposition == DuplicatePairingHelloDisposition)
	{
		KSessionTraceLogger::write(m_bOutgoing
				? QStringLiteral("controller") : QStringLiteral("controlled"),
			QStringLiteral("pairing_hello"), QStringLiteral("duplicate"), -1,
			QStringLiteral("requestId=%1 generation=%2")
				.arg(message.strRequestId).arg(m_nGeneration));
		replayCurrentPairingResponse();
		return true;
	}
	if (message.strVerificationMethod
		!= KTlsPairingVerification::verificationMethod())
	{
		fail(QStringLiteral("protocol_incompatible"), true,
			PairingHelloSecurityStage);
		return true;
	}
	if (message.strDeviceId != m_securePeer.strDeviceId)
	{
		fail(QStringLiteral("device_key_changed"), true,
			PairingHelloSecurityStage);
		return true;
	}
	if (!m_bOutgoing)
	{
		m_context.strRequestId = message.strRequestId;
		m_context.requestedPermissions = message.permissions;
		if (!transaction().isActive())
			m_pPairingCommand->begin(message.strRequestId, m_nGeneration);
	}
	else if (!transaction().matches(message.strRequestId, m_nGeneration))
	{
		return false;
	}
	if (!transaction().markHelloReceived())
		return false;
	m_pPairingCommand->acceptHello(message);
	m_context.strRemoteDeviceName = message.strDeviceName;
	QString strReason;
	if (!inspectPeerTrust(message.strTrustCommitId, &strReason))
	{
		fail(strReason, true, PairingHelloSecurityStage);
		return true;
	}
	if (!transaction().helloSent())
		sendHello();
	m_pTimer->start(m_nApprovalTimeoutSeconds * 1000);
	beginPairingOrAutomaticDecision();
	return true;
}

bool KDeviceAuthenticationFlow::handlePairingDecision(
	const KTlsPairingMessage &message)
{
	if (!transaction().helloReceived()
		|| message.strDeviceId != m_securePeer.strDeviceId)
		return false;
	if (!message.bAccepted)
	{
		fail(QStringLiteral("pairing_rejected"), false,
			UserApprovalSecurityStage);
		return true;
	}
	if (!transaction().markRemoteDecision(message.permissions))
		return false;
	tryPrepare();
	return true;
}

bool KDeviceAuthenticationFlow::handlePrepared(
	const KTlsPairingMessage &message)
{
	const KPermissionScopes effective = m_bOutgoing
		? transaction().remotePermissions() : transaction().localPermissions();
	if (message.strDeviceId != m_securePeer.strDeviceId
		|| message.permissions != effective)
	{
		fail(QStringLiteral("protocol_incompatible"), true,
			PrepareSecurityStage);
		return true;
	}
	if (!transaction().markRemotePrepared())
		return false;
	tryPrepare();
	tryCommit();
	return true;
}

bool KDeviceAuthenticationFlow::handleCommitted(
	const KTlsPairingMessage &message)
{
	if (message.strDeviceId != m_securePeer.strDeviceId
		|| message.permissions != m_context.effectivePermissions
		|| !transaction().markRemoteCommitted())
	{
		return false;
	}
	tryCommit();
	tryComplete();
	return true;
}

bool KDeviceAuthenticationFlow::loadTrust(QString *pErrorMessage)
{
	if (!m_pIdentityProvider->initialize(pErrorMessage))
		return false;
	m_localCertificate = m_pIdentityProvider->certificate();
	QString strStoreError;
	if (!m_trustedDeviceService.load(&strStoreError))
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

bool KDeviceAuthenticationFlow::inspectPeerTrust(
	const QString &strPeerCommitId,
	QString *pReason)
{
	const KTrustedDevice *pTrusted = m_trustedDeviceService.find(
		m_context.strRemoteDeviceId);
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
	m_context.bTrustedDevice = m_trustedDeviceService.isMutuallyTrusted(
		m_context.strRemoteDeviceId, m_securePeer.spkiSha256,
		strPeerCommitId);
	if (!m_context.bTrustedDevice)
	{
		m_context.bRequestWithinTrust = false;
		return true;
	}
	// File transfer was introduced after the original trust-store schema. Treat it
	// as an optional capability when deciding whether an existing trusted device
	// may use automatic access; the final permission intersection still removes it.
	KPermissionScopes accessPermissions = m_context.requestedPermissions;
	accessPermissions.setFlag(FileTransferPermissionScope, false);
	m_context.bRequestWithinTrust =
		(accessPermissions & ~pTrusted->permissionLimit).toInt() == 0;
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
	hello.strTrustCommitId = m_trustedDeviceService.mutualCommitId(
		m_context.strRemoteDeviceId, m_securePeer.spkiSha256);
	hello.permissions = m_context.requestedPermissions;
	if (!transaction().markHelloSent())
		return;
	sendPairingMessage(hello);
}

void KDeviceAuthenticationFlow::beginPairingOrAutomaticDecision()
{
	if (!transaction().helloReceived()
		|| transaction().localPermissions().toInt() != 0
		|| m_bPairingPromptVisible)
		return;
	const KTrustedDevice *pTrusted = m_trustedDeviceService.find(
		m_context.strRemoteDeviceId);
	if (m_context.bTrustedDevice && pTrusted != nullptr)
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
		fail(QStringLiteral("channel_binding_unavailable"), true,
			PairingHelloSecurityStage, strError);
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
	if (!bAccepted || !transaction().markLocalDecision(permissions))
		return;
	KTlsPairingMessage decision;
	decision.type = DecisionTlsPairingMessageType;
	decision.strRequestId = m_context.strRequestId;
	decision.strDeviceId = m_localCertificate.strDeviceId;
	decision.bAccepted = bAccepted;
	decision.permissions = permissions;
	sendPairingMessage(decision);
	tryPrepare();
}

void KDeviceAuthenticationFlow::sendPairingMessage(
	const KTlsPairingMessage &message, bool bCache)
{
	if (bCache)
		m_pPairingCommand->cacheResponse(message);
	emit messageReady(message);
}

void KDeviceAuthenticationFlow::replayCurrentPairingResponse()
{
	const std::optional<KTlsPairingMessage> response =
		m_pPairingCommand->replayResponse();
	if (response.has_value())
		emit messageReady(response.value());
}

void KDeviceAuthenticationFlow::tryPrepare()
{
	if (!transaction().decisionsComplete()
		|| transaction().localPrepared())
	{
		return;
	}
	const KPermissionScopes effective = m_bOutgoing
		? transaction().remotePermissions() : transaction().localPermissions();
	if (!effective.testFlag(ViewScreenPermissionScope))
	{
		fail(QStringLiteral("pairing_rejected"), true,
			UserApprovalSecurityStage);
		return;
	}
	m_context.effectivePermissions = effective;
	if (!m_context.bTrustedDevice)
	{
		QString strError;
		if (!m_trustedDeviceService.prepare(m_context.strRequestId,
			m_securePeer, m_context.strRemoteDeviceName, effective, &strError))
		{
			fail(QStringLiteral("identity_unavailable"), true,
				PrepareSecurityStage, strError);
			return;
		}
	}
	if (!transaction().markLocalPrepared())
		return;
	KTlsPairingMessage ready;
	ready.type = ReadyTlsPairingMessageType;
	ready.strRequestId = m_context.strRequestId;
	ready.strDeviceId = m_localCertificate.strDeviceId;
	ready.permissions = effective;
	sendPairingMessage(ready);
	tryCommit();
	tryComplete();
}

void KDeviceAuthenticationFlow::tryCommit()
{
	if (!transaction().preparedComplete()
		|| transaction().localCommitted())
	{
		return;
	}
	if (!m_context.bTrustedDevice)
	{
		QString strError;
		if (!m_trustedDeviceService.commit(m_context.strRequestId,
			m_context.strRemoteDeviceName, m_securePeer.certificateSha256,
			&strError))
		{
			fail(QStringLiteral("identity_unavailable"), true,
				CommitSecurityStage, strError);
			return;
		}
	}
	if (!transaction().markLocalCommitted())
		return;
	KTlsPairingMessage committed;
	committed.type = CommittedTlsPairingMessageType;
	committed.strRequestId = m_context.strRequestId;
	committed.strDeviceId = m_localCertificate.strDeviceId;
	committed.permissions = m_context.effectivePermissions;
	sendPairingMessage(committed);
	tryComplete();
}

void KDeviceAuthenticationFlow::tryComplete()
{
	if (!transaction().committedComplete() || m_bAuthenticated)
	{
		return;
	}
	if (m_context.bTrustedDevice)
	{
		QString strError;
		if (!m_trustedDeviceService.updateAuthenticated(
			m_context.strRemoteDeviceId, m_context.strRemoteDeviceName,
			m_securePeer.certificateSha256, &strError))
		{
			KSessionTraceLogger::write(m_bOutgoing
					? QStringLiteral("controller") : QStringLiteral("controlled"),
				QStringLiteral("authentication_metadata"),
				QStringLiteral("save_failed"), -1,
				QStringLiteral("requestId=%1 deviceId=%2 error=%3")
					.arg(m_context.strRequestId,
						m_context.strRemoteDeviceId, strError));
		}
	}
	if (!transaction().markCompleted())
		return;
	QString strCleanupError;
	if (!m_trustedDeviceService.complete(m_context.strRequestId,
		&strCleanupError))
	{
		KSessionTraceLogger::write(m_bOutgoing
				? QStringLiteral("controller") : QStringLiteral("controlled"),
			QStringLiteral("pairing_cleanup"), QStringLiteral("save_failed"), -1,
			QStringLiteral("requestId=%1 deviceId=%2 error=%3")
				.arg(m_context.strRequestId,
					m_context.strRemoteDeviceId, strCleanupError));
	}
	m_context.bTrustedDevice = true;
	m_context.bRequestWithinTrust = true;
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

void KDeviceAuthenticationFlow::rollbackPairing(const QString &strReason)
{
	if (!m_trustedDeviceService.hasTransaction(m_context.strRequestId))
		return;
	QString strRollbackError;
	if (!m_trustedDeviceService.rollback(m_context.strRequestId,
		&strRollbackError))
	{
		KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
				: QStringLiteral("controlled"),
			QStringLiteral("pairing_rollback_failed"), QStringLiteral("error"), -1,
			QStringLiteral("requestId=%1 reason=%2 error=%3")
				.arg(m_context.strRequestId, strReason, strRollbackError));
	}
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

bool KDeviceAuthenticationFlow::isSourceRateLimited()
{
	return m_pAdmissionController != nullptr
		&& m_pAdmissionController->isRateLimited(m_strSourceAddress);
}

void KDeviceAuthenticationFlow::recordSourceFailure()
{
	if (m_bOutgoing || m_strSourceAddress.isEmpty())
		return;
	if (m_pAdmissionController != nullptr)
		m_pAdmissionController->recordPeerFailure(m_strSourceAddress);
}

void KDeviceAuthenticationFlow::fail(const QString &strReason,
	bool bNotifyRemote,
	KSecurityStage stage,
	const QString &strTechnicalMessage)
{
	KSecurityStatus status = KSecurityStatus::fromProtocolReason(
		strReason, stage, strTechnicalMessage);
	if (!status.isValid())
	{
		status = KSecurityStatus::fromProtocolReason(QStringLiteral("cancelled"),
			stage, strTechnicalMessage.isEmpty() ? strReason : strTechnicalMessage);
	}
	status.strRequestId = m_context.strRequestId;
	status.nGeneration = m_nGeneration;
	status.bOutgoing = m_bOutgoing;
	rollbackPairing(strReason);
	transaction().markFailed();
	if (ShouldRecordSourceFailure(strReason))
		recordSourceFailure();
	KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
			: QStringLiteral("controlled"),
		QStringLiteral("authentication"), QStringLiteral("rejected"), -1,
		QStringLiteral("requestId=%1 generation=%2 domain=%3 code=%4 stage=%5 "
			"deviceId=%6 fingerprint=%7 technical=%8")
			.arg(m_context.strRequestId)
			.arg(m_nGeneration)
			.arg(KSecurityStatus::domainName(status.domain))
			.arg(KSecurityStatus::codeName(status.code))
			.arg(KSecurityStatus::stageName(status.stage))
			.arg(m_context.strRemoteDeviceId)
			.arg(QString::fromLatin1(m_securePeer.spkiSha256.toHex().left(12)))
			.arg(status.strTechnicalMessage));
	const QString strRequestId = m_context.strRequestId;
	if (bNotifyRemote && !strRequestId.isEmpty())
	{
		KTlsPairingMessage rejected;
		rejected.type = RejectedTlsPairingMessageType;
		rejected.strRequestId = strRequestId;
		rejected.strReason = KTlsPairingMessageCodec::isValidRejectReason(strReason)
			? strReason : QStringLiteral("cancelled");
		sendPairingMessage(rejected);
	}
	clear(strReason);
	emit authenticationRejected(status);
}

void KDeviceAuthenticationFlow::clear(const QString &strReason,
	bool bKeepPeerIdentity)
{
	rollbackPairing(strReason);
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
	m_pPairingCommand->reset();
	m_nGeneration = 0;
	m_bOutgoing = false;
	m_bActive = false;
	m_bAuthenticated = false;
	m_bPairingPromptVisible = false;
}

KPairingTransaction &KDeviceAuthenticationFlow::transaction()
{
	return m_pPairingCommand->transaction();
}

const KPairingTransaction &KDeviceAuthenticationFlow::transaction() const
{
	return m_pPairingCommand->transaction();
}
