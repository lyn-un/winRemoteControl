#include "fakesecurity.h"
#include "core/security/tlspairingverification.h"
#include "core/security/sourcefailuretracker.h"
#include "session/deviceauthenticationflow.h"
#include "session/trusteddeviceservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTextStream>
#include <QtCore/QUuid>

#include <algorithm>

namespace
{
	bool Require(bool bCondition, const QString &strMessage)
	{
		if (bCondition)
			return true;
		QTextStream(stderr) << strMessage << Qt::endl;
		return false;
	}

	bool RequireSecuritySuccess(const KSecurityStatus &status,
		const QString &strContext)
	{
		return Require(!status.isValid(),
			QStringLiteral("%1: %2 (%3)").arg(strContext,
				status.strProtocolReason, status.strTechnicalMessage));
	}

	KTlsPeerIdentity PeerIdentity(const KFakeDeviceIdentityProvider &provider)
	{
		const KDeviceCertificate certificate = provider.certificate();
		KTlsPeerIdentity peer;
		peer.strDeviceId = certificate.strDeviceId;
		peer.spkiSha256 = certificate.spkiSha256;
		peer.certificateSha256 = certificate.certificateSha256;
		peer.validFromUtc = certificate.validFromUtc;
		peer.validToUtc = certificate.validToUtc;
		peer.strTlsProtocol = QStringLiteral("TLS1.3");
		peer.strCipherSuite = QStringLiteral("TLS_AES_256_GCM_SHA384");
		return peer;
	}

	bool TestVerificationMethodAndExporterFailure()
	{
		KFakeDeviceIdentityProvider localIdentity;
		KFakeDeviceIdentityProvider remoteIdentity;
		KFakeTrustedDeviceStore store;
		KFakeKeyingMaterialExporter exporter;
		store.setIdentityProvider(&localIdentity);
		KDeviceAuthenticationFlow flow(&localIdentity, &store, &exporter);
		flow.setSecurePeerIdentity(PeerIdentity(remoteIdentity));
		QString strRejectedReason;
		QObject::connect(&flow, &KDeviceAuthenticationFlow::authenticationRejected,
			[&strRejectedReason](const KSecurityStatus &status)
			{ strRejectedReason = status.strProtocolReason; });
		if (!RequireSecuritySuccess(flow.beginIncoming(
			QStringLiteral("192.0.2.10"), 1, QStringLiteral("local")),
			QStringLiteral("Begin incoming verification-method test")))
		{
			return false;
		}
		KTlsPairingMessage hello;
		hello.type = HelloTlsPairingMessageType;
		hello.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		hello.strDeviceId = remoteIdentity.identity().strDeviceId;
		hello.strDeviceName = QStringLiteral("remote");
		hello.strVerificationMethod = QStringLiteral("legacy-fingerprint-v1");
		hello.permissions = ViewScreenPermissionScope;
		flow.handleMessage(hello, 1);
		if (!Require(strRejectedReason == QStringLiteral("protocol_incompatible"),
			QStringLiteral("Unknown pairing verification method was not rejected")))
		{
			return false;
		}

		strRejectedReason.clear();
		flow.setSecurePeerIdentity(PeerIdentity(remoteIdentity));
		exporter.setFailure(true);
		if (!RequireSecuritySuccess(flow.beginIncoming(
			QStringLiteral("192.0.2.10"), 2, QStringLiteral("local")),
			QStringLiteral("Begin incoming exporter-failure test")))
		{
			return false;
		}
		hello.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		hello.strVerificationMethod = KTlsPairingVerification::verificationMethod();
		flow.handleMessage(hello, 2);
		return Require(strRejectedReason
			== QStringLiteral("channel_binding_unavailable"),
			QStringLiteral("TLS exporter failure did not close pairing safely"));
	}

	KTrustedDevice TrustedDeviceFor(const KFakeDeviceIdentityProvider &identity,
		KPermissionScopes permissions)
	{
		const KDeviceCertificate certificate = identity.certificate();
		KTrustedDevice device;
		device.strDeviceId = certificate.strDeviceId;
		device.spkiSha256 = certificate.spkiSha256;
		device.certificateSha256 = certificate.certificateSha256;
		device.strFingerprint = certificate.spkiFingerprint();
		device.strAlias = QStringLiteral("existing");
		device.strAdvertisedName = QStringLiteral("existing");
		device.permissionLimit = permissions;
		device.nPairedAtMs = 100;
		device.nLastAuthenticatedAtMs = 200;
		device.commitState = MutualTrustedDeviceCommitState;
		device.strPairingTransactionId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		return device;
	}

	bool TestTrustedDeviceTransactionRollback()
	{
		const KPermissionScopes permissions = KPermissionScopes::fromInt(
			kAllPermissionScopeBits);
		KFakeDeviceIdentityProvider localIdentity;
		KFakeDeviceIdentityProvider remoteIdentity;
		KFakeTrustedDeviceStore store;
		store.setIdentityProvider(&localIdentity);
		const KTrustedDevice original = TrustedDeviceFor(remoteIdentity, permissions);
		store.setDevices({ original });
		KTrustedDeviceService service(&store);
		QString strError;
		if (!Require(service.load(&strError), strError))
			return false;

		KTlsPeerIdentity peer = PeerIdentity(remoteIdentity);
		peer.certificateSha256 = QByteArray(32, 'n');
		const QString strRequestId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		if (!Require(service.prepare(strRequestId, peer, QStringLiteral("updated"),
			ViewScreenPermissionScope, &strError), strError)
			|| !Require(store.devices().size() == 2
				&& store.devices().at(0).commitState
					== MutualTrustedDeviceCommitState
				&& store.devices().at(1).commitState
					== PendingTrustedDeviceCommitState,
				QStringLiteral("Prepare overwrote the existing mutual trust record"))
			|| !Require(service.rollback(strRequestId, &strError), strError)
			|| !Require(store.devices().size() == 1
				&& store.devices().first().strAlias == original.strAlias
				&& store.devices().first().certificateSha256
					== original.certificateSha256
				&& store.devices().first().permissionLimit == original.permissionLimit
				&& store.devices().first().commitState
					== MutualTrustedDeviceCommitState,
				QStringLiteral("Rollback did not restore the original trusted record")))
		{
			return false;
		}

		const QString strCrashRequestId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		if (!Require(service.prepare(strCrashRequestId, peer,
			QStringLiteral("crash-update"), ViewScreenPermissionScope,
			&strError), strError))
		{
			return false;
		}
		KTrustedDeviceService restartedAfterPrepare(&store);
		if (!Require(restartedAfterPrepare.load(&strError), strError)
			|| !Require(store.devices().size() == 1
				&& store.devices().first().commitState
					== MutualTrustedDeviceCommitState
				&& store.devices().first().strAlias == original.strAlias
				&& store.devices().first().permissionLimit == original.permissionLimit,
				QStringLiteral("Restart did not recover the pre-transaction trust record")))
		{
			return false;
		}

		const QString strCommittedRequestId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		if (!Require(restartedAfterPrepare.prepare(strCommittedRequestId, peer,
			QStringLiteral("prepared"), ViewScreenPermissionScope, &strError), strError)
			|| !Require(restartedAfterPrepare.commit(strCommittedRequestId,
				QStringLiteral("committed"), peer.certificateSha256, &strError), strError)
			|| !Require(store.devices().size() == 2,
				QStringLiteral("Commit discarded the durable rollback record")))
		{
			return false;
		}
		const auto committedIterator = std::find_if(store.devices().cbegin(),
			store.devices().cend(), [&strCommittedRequestId](const KTrustedDevice &device)
			{ return device.strPairingTransactionId == strCommittedRequestId; });
		if (!Require(committedIterator != store.devices().cend()
				&& committedIterator->strAdvertisedName == QStringLiteral("committed")
				&& committedIterator->certificateSha256 == peer.certificateSha256
				&& committedIterator->nLastAuthenticatedAtMs > 0
				&& committedIterator->commitState == MutualTrustedDeviceCommitState,
			QStringLiteral("Commit did not persist final authentication metadata")))
		{
			return false;
		}

		// A crash after the local Commit but before both Committed messages must
		// recover the previously confirmed Mutual record, not the new half-commit.
		KTrustedDeviceService restartedAfterLocalCommit(&store);
		if (!Require(restartedAfterLocalCommit.load(&strError), strError)
			|| !Require(store.devices().size() == 1
				&& store.devices().first().strPairingTransactionId
					== original.strPairingTransactionId
				&& store.devices().first().strAlias == original.strAlias
				&& store.devices().first().permissionLimit == original.permissionLimit,
				QStringLiteral("Restart accepted a one-sided committed trust update")))
		{
			return false;
		}

		const QString strCompletedRequestId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		if (!Require(restartedAfterLocalCommit.prepare(strCompletedRequestId, peer,
			QStringLiteral("prepared"), ViewScreenPermissionScope, &strError), strError)
			|| !Require(restartedAfterLocalCommit.commit(strCompletedRequestId,
				QStringLiteral("committed"), peer.certificateSha256, &strError), strError)
			|| !Require(restartedAfterLocalCommit.complete(strCompletedRequestId,
				&strError), strError)
			|| !Require(store.devices().size() == 1
				&& store.devices().first().strPairingTransactionId
					== strCompletedRequestId
				&& store.devices().first().strAdvertisedName
					== QStringLiteral("committed"),
				QStringLiteral("Completed update retained the rollback record")))
		{
			return false;
		}

		KTrustedDevice pending = original;
		pending.commitState = PendingTrustedDeviceCommitState;
		pending.strPairingTransactionId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		store.setDevices({ original, pending });
		KTrustedDeviceService restartedWithSyntheticPending(&store);
		if (!Require(restartedWithSyntheticPending.load(&strError), strError)
			|| !Require(store.devices().size() == 1
				&& store.devices().first().commitState
					== MutualTrustedDeviceCommitState,
				QStringLiteral("Restart recovery did not remove pending trust")))
		{
			return false;
		}
		return true;
	}

	bool TestCommitFailureRollsBackBothPeers()
	{
		KFakeDeviceIdentityProvider controllerIdentity;
		KFakeDeviceIdentityProvider controlledIdentity;
		KFakeTrustedDeviceStore controllerStore;
		KFakeTrustedDeviceStore controlledStore;
		KFakeKeyingMaterialExporter exporter;
		controllerStore.setIdentityProvider(&controllerIdentity);
		controlledStore.setIdentityProvider(&controlledIdentity);
		// Save 1 prepares the pending record; save 2 attempts to commit it.
		controlledStore.failOnSaveCall(2);
		KDeviceAuthenticationFlow controller(&controllerIdentity, &controllerStore,
			&exporter);
		KDeviceAuthenticationFlow controlled(&controlledIdentity, &controlledStore,
			&exporter);
		controller.setSecurePeerIdentity(PeerIdentity(controlledIdentity));
		controlled.setSecurePeerIdentity(PeerIdentity(controllerIdentity));
		constexpr quint64 kGeneration = 31;
		QObject::connect(&controller, &KDeviceAuthenticationFlow::messageReady,
			[&controlled](const KTlsPairingMessage &message)
			{ controlled.handleMessage(message, kGeneration); });
		QObject::connect(&controlled, &KDeviceAuthenticationFlow::messageReady,
			[&controller](const KTlsPairingMessage &message)
			{ controller.handleMessage(message, kGeneration); });
		QString strControllerRequestId;
		QString strControlledRequestId;
		QObject::connect(&controller, &KDeviceAuthenticationFlow::pairingRequested,
			[&](const QString &strRequestId, const QString &, const QString &,
				const QString &, const QString &, const QString &, const QString &,
				const QString &, KPermissionScopes, qint64)
			{ strControllerRequestId = strRequestId; });
		QObject::connect(&controlled, &KDeviceAuthenticationFlow::pairingRequested,
			[&](const QString &strRequestId, const QString &, const QString &,
				const QString &, const QString &, const QString &, const QString &,
				const QString &, KPermissionScopes, qint64)
			{ strControlledRequestId = strRequestId; });
		int nAuthenticated = 0;
		QObject::connect(&controller, &KDeviceAuthenticationFlow::authenticationSucceeded,
			[&](const KDeviceAuthenticationContext &) { ++nAuthenticated; });
		QObject::connect(&controlled, &KDeviceAuthenticationFlow::authenticationSucceeded,
			[&](const KDeviceAuthenticationContext &) { ++nAuthenticated; });

		const KPermissionScopes permissions = KPermissionScopes::fromInt(
			kAllPermissionScopeBits);
		if (!RequireSecuritySuccess(controlled.beginIncoming(
			QStringLiteral("192.0.2.1"), kGeneration,
			QStringLiteral("controlled")), QStringLiteral("Begin controlled"))
			|| !RequireSecuritySuccess(controller.beginOutgoing(
				QUuid::createUuid().toString(QUuid::WithoutBraces), kGeneration,
				QStringLiteral("controller"), permissions),
				QStringLiteral("Begin controller")))
		{
			return false;
		}
		controller.respondPairing(strControllerRequestId, true, permissions);
		controlled.respondPairing(strControlledRequestId, true, permissions);
		return Require(nAuthenticated == 0,
				QStringLiteral("A peer authenticated after the other commit failed"))
			&& Require(controllerStore.devices().isEmpty()
				&& controlledStore.devices().isEmpty(),
				QStringLiteral("Commit failure left a half-paired trust record"));
	}

	bool TestSourceFailureTrackerBoundsAndExpiry()
	{
		KSourceFailureTracker tracker(3, 100, 4);
		for (int nIndex = 0; nIndex < 3; ++nIndex)
			tracker.recordFailure(QStringLiteral("source-a"), nIndex * 10);
		if (!Require(tracker.isRateLimited(QStringLiteral("source-a"), 20),
			QStringLiteral("Source failure threshold did not rate-limit")))
		{
			return false;
		}
		for (int nIndex = 0; nIndex < 10; ++nIndex)
		{
			tracker.recordFailure(QStringLiteral("source-%1").arg(nIndex),
				30 + nIndex);
		}
		return Require(tracker.trackedSourceCount() <= 4,
				QStringLiteral("Source failure tracker exceeded its global bound"))
			&& Require(!tracker.isRateLimited(QStringLiteral("source-a"), 200),
				QStringLiteral("Expired source failures did not leave the window"));
	}

	bool TestRateLimitPrecedesIdentityAndTrustLoad()
	{
		KFakeDeviceIdentityProvider localIdentity;
		KFakeDeviceIdentityProvider remoteIdentity;
		KFakeTrustedDeviceStore store;
		KFakeKeyingMaterialExporter exporter;
		store.setIdentityProvider(&localIdentity);
		KDeviceAuthenticationFlow flow(&localIdentity, &store, &exporter);
		flow.setSecurePeerIdentity(PeerIdentity(remoteIdentity));
		for (quint64 nGeneration = 1; nGeneration <= 5; ++nGeneration)
		{
			if (!RequireSecuritySuccess(flow.beginIncoming(
				QStringLiteral(" 192.0.2.44 "), nGeneration,
				QStringLiteral("local")), QStringLiteral("Build rate limit")))
			{
				return false;
			}
			KTlsPairingMessage hello;
			hello.type = HelloTlsPairingMessageType;
			hello.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			hello.strDeviceId = remoteIdentity.identity().strDeviceId;
			hello.strDeviceName = QStringLiteral("remote");
			hello.strVerificationMethod = QStringLiteral("unsupported");
			hello.permissions = ViewScreenPermissionScope;
			flow.handleMessage(hello, nGeneration);
			flow.setSecurePeerIdentity(PeerIdentity(remoteIdentity));
		}
		const int nInitializeCount = localIdentity.initializeCount();
		const int nLoadCount = store.loadCount();
		const KSecurityStatus status = flow.beginIncoming(
			QStringLiteral("192.0.2.44"), 6, QStringLiteral("local"));
		return Require(status.code == PairingRateLimitedSecurityErrorCode
			&& status.stage == PairingHelloSecurityStage,
			QStringLiteral("Rate-limited source was not rejected at admission"))
			&& Require(localIdentity.initializeCount() == nInitializeCount
				&& store.loadCount() == nLoadCount,
				QStringLiteral("Rate-limited source reached identity or trust loading"));
	}

	bool TestStructuredSecurityStatus()
	{
		const KSecurityStatus changed = KSecurityStatus::fromProtocolReason(
			QStringLiteral("device_key_changed"), PairingHelloSecurityStage,
			QStringLiteral("test detail"));
		const KSecurityStatus timeout = KSecurityStatus::fromProtocolReason(
			QStringLiteral("authentication_timeout"), UserApprovalSecurityStage);
		return Require(changed.isValid()
				&& changed.domain == PeerCertificateSecurityErrorDomain
				&& changed.code == DeviceKeyChangedSecurityErrorCode
				&& changed.bRequiresRePair && changed.bUserActionRequired
				&& changed.strProtocolReason == QStringLiteral("device_key_changed"),
				QStringLiteral("Device key change status is not stable and structured"))
			&& Require(timeout.isValid() && timeout.bRetryable
				&& timeout.stage == UserApprovalSecurityStage,
				QStringLiteral("Authentication timeout retry metadata is incorrect"));
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	if (!TestVerificationMethodAndExporterFailure())
		return 1;
	if (!TestTrustedDeviceTransactionRollback())
		return 1;
	if (!TestCommitFailureRollsBackBothPeers())
		return 1;
	if (!TestSourceFailureTrackerBoundsAndExpiry())
		return 1;
	if (!TestRateLimitPrecedesIdentityAndTrustLoad())
		return 1;
	if (!TestStructuredSecurityStatus())
		return 1;
	QString strVerificationError;
	QByteArray leadingZeroMaterial(32, '\0');
	leadingZeroMaterial[3] = 42;
	if (!Require(KTlsPairingVerification::numericCode(
		leadingZeroMaterial, &strVerificationError) == QStringLiteral("000 042"),
		QStringLiteral("TLS numeric pairing code does not preserve leading zeroes")))
	{
		return 1;
	}
	const QString strContextRequestId = QUuid::createUuid().toString(
		QUuid::WithoutBraces);
	const QString strContextControllerId = QUuid::createUuid().toString(
		QUuid::WithoutBraces);
	const QString strContextControlledId = QUuid::createUuid().toString(
		QUuid::WithoutBraces);
	const QByteArray controllerSpki(32, 'c');
	const QByteArray controlledSpki(32, 'd');
	const QByteArray canonicalContext = KTlsPairingVerification::createContext(
		strContextRequestId, strContextControllerId, strContextControlledId,
		controllerSpki, controlledSpki, &strVerificationError);
	const QByteArray reversedContext = KTlsPairingVerification::createContext(
		strContextRequestId, strContextControlledId, strContextControllerId,
		controlledSpki, controllerSpki, &strVerificationError);
	if (!Require(!canonicalContext.isEmpty()
		&& canonicalContext != reversedContext,
		QStringLiteral("TLS pairing context does not preserve role ordering")))
	{
		return 1;
	}
	KFakeDeviceIdentityProvider controllerIdentity;
	KFakeDeviceIdentityProvider controlledIdentity;
	KFakeTrustedDeviceStore controllerStore;
	KFakeTrustedDeviceStore controlledStore;
	KFakeKeyingMaterialExporter keyingMaterialExporter;
	controllerStore.setIdentityProvider(&controllerIdentity);
	controlledStore.setIdentityProvider(&controlledIdentity);
	KDeviceAuthenticationFlow controller(&controllerIdentity, &controllerStore,
		&keyingMaterialExporter);
	KDeviceAuthenticationFlow controlled(&controlledIdentity, &controlledStore,
		&keyingMaterialExporter);
	controller.setSecurePeerIdentity(PeerIdentity(controlledIdentity));
	controlled.setSecurePeerIdentity(PeerIdentity(controllerIdentity));
	constexpr quint64 kGeneration = 7;
	quint64 nCurrentGeneration = kGeneration;
	KTlsPairingMessage controllerHello;
	KTlsPairingMessage controllerReady;
	KTlsPairingMessage controllerCommitted;
	QObject::connect(&controller, &KDeviceAuthenticationFlow::messageReady,
		[&](const KTlsPairingMessage &message)
		{
			if (message.type == HelloTlsPairingMessageType)
				controllerHello = message;
			else if (message.type == ReadyTlsPairingMessageType)
				controllerReady = message;
			else if (message.type == CommittedTlsPairingMessageType)
				controllerCommitted = message;
			controlled.handleMessage(message, nCurrentGeneration);
		});
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::messageReady,
		[&controller, &nCurrentGeneration](const KTlsPairingMessage &message)
		{ controller.handleMessage(message, nCurrentGeneration); });

	QString strControllerControllerFingerprint;
	QString strControllerControlledFingerprint;
	QString strControlledControllerFingerprint;
	QString strControlledControlledFingerprint;
	QString strControllerVerificationCode;
	QString strControlledVerificationCode;
	QString strControllerRequestId;
	QString strControlledRequestId;
	int nControllerPairingCount = 0;
	int nControlledPairingCount = 0;
	int nControllerAuthenticated = 0;
	int nControlledAuthenticated = 0;
	QString strControllerRejected;
	QString strControlledRejected;
	QObject::connect(&controller, &KDeviceAuthenticationFlow::pairingRequested,
		[&](const QString &strRequestId, const QString &,
			const QString &, const QString &strVerificationCode,
			const QString &strControllerFingerprint,
			const QString &strControlledFingerprint,
			const QString &, const QString &, KPermissionScopes, qint64)
		{
			strControllerRequestId = strRequestId;
			strControllerVerificationCode = strVerificationCode;
			strControllerControllerFingerprint = strControllerFingerprint;
			strControllerControlledFingerprint = strControlledFingerprint;
			++nControllerPairingCount;
		});
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::pairingRequested,
		[&](const QString &strRequestId, const QString &,
			const QString &, const QString &strVerificationCode,
			const QString &strControllerFingerprint,
			const QString &strControlledFingerprint,
			const QString &, const QString &, KPermissionScopes, qint64)
		{
			strControlledRequestId = strRequestId;
			strControlledVerificationCode = strVerificationCode;
			strControlledControllerFingerprint = strControllerFingerprint;
			strControlledControlledFingerprint = strControlledFingerprint;
			++nControlledPairingCount;
		});
	QObject::connect(&controller, &KDeviceAuthenticationFlow::authenticationSucceeded,
		[&](const KDeviceAuthenticationContext &) { ++nControllerAuthenticated; });
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::authenticationSucceeded,
		[&](const KDeviceAuthenticationContext &) { ++nControlledAuthenticated; });
	QObject::connect(&controller, &KDeviceAuthenticationFlow::authenticationRejected,
		[&](const KSecurityStatus &status)
		{ strControllerRejected = status.strProtocolReason; });
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::authenticationRejected,
		[&](const KSecurityStatus &status)
		{ strControlledRejected = status.strProtocolReason; });

	if (!RequireSecuritySuccess(controlled.beginIncoming(
		QStringLiteral("192.0.2.1"), kGeneration,
		QStringLiteral("controlled")), QStringLiteral("Begin controlled pairing")))
	{
		return 1;
	}
	const QString strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const KPermissionScopes allPermissions = KPermissionScopes::fromInt(kAllPermissionScopeBits);
	if (!RequireSecuritySuccess(controller.beginOutgoing(strRequestId, kGeneration,
		QStringLiteral("controller"), allPermissions),
		QStringLiteral("Begin controller pairing"))
		|| !Require(nControllerPairingCount == 1 && nControlledPairingCount == 1,
			QStringLiteral("First authentication did not request pairing on both peers"))
		|| !Require(strControllerControllerFingerprint
				== strControlledControllerFingerprint
			&& strControllerControlledFingerprint
				== strControlledControlledFingerprint
			&& strControllerControllerFingerprint.startsWith(QStringLiteral("SHA256:")),
			QStringLiteral("TLS certificate fingerprints do not match"))
		|| !Require(strControllerVerificationCode == strControlledVerificationCode
			&& strControllerVerificationCode.size() == 7
			&& strControllerVerificationCode.at(3) == QLatin1Char(' '),
			QStringLiteral("TLS pairing verification codes do not match")))
	{
		return 1;
	}
	if (!Require(controlled.handleMessage(controllerHello, kGeneration),
		QStringLiteral("Duplicate Hello was not handled idempotently"))
		|| !Require(nControllerPairingCount == 1 && nControlledPairingCount == 1
			&& controlled.context().strRequestId == strRequestId,
			QStringLiteral("Duplicate Hello reset the active pairing transaction")))
	{
		return 1;
	}
	KTlsPairingMessage conflictingHello = controllerHello;
	conflictingHello.strRequestId = QUuid::createUuid().toString(
		QUuid::WithoutBraces);
	if (!Require(controlled.handleMessage(conflictingHello, kGeneration),
		QStringLiteral("Conflicting Hello was not rejected"))
		|| !Require(controlled.context().strRequestId == strRequestId
			&& nControlledPairingCount == 1,
			QStringLiteral("Conflicting Hello replaced the active transaction")))
	{
		return 1;
	}
	controller.respondPairing(strControllerRequestId, true, allPermissions);
	controlled.respondPairing(strControlledRequestId, true, allPermissions);
	if (!Require(nControllerAuthenticated == 1 && nControlledAuthenticated == 1,
		QStringLiteral("Paired peers did not authenticate"))
		|| !Require(controller.context().effectivePermissions == allPermissions,
			QStringLiteral("Negotiated permissions are incorrect"))
		|| !Require(controllerStore.saveCount() == 2
			&& controlledStore.saveCount() == 2,
			QStringLiteral("Pairing performed a required write after Committed")))
	{
		return 1;
	}
	const int nControlledSaveCount = controlledStore.saveCount();
	if (!Require(!controlled.handleMessage(controllerReady, kGeneration)
		&& !controlled.handleMessage(controllerCommitted, kGeneration)
		&& controlledStore.saveCount() == nControlledSaveCount,
		QStringLiteral("Late pairing messages repeated a completed trust write")))
	{
		return 1;
	}

	controllerStore.failOnSaveCall(3);
	controlledStore.failOnSaveCall(3);
	nCurrentGeneration = kGeneration + 1;
	if (!RequireSecuritySuccess(controlled.beginIncoming(
		QStringLiteral("192.0.2.1"), kGeneration + 1,
		QStringLiteral("controlled")), QStringLiteral("Begin trusted controlled"))
		|| !RequireSecuritySuccess(controller.beginOutgoing(
			QUuid::createUuid().toString(QUuid::WithoutBraces), kGeneration + 1,
			QStringLiteral("controller"), allPermissions),
			QStringLiteral("Begin trusted controller"))
		|| !Require(nControllerPairingCount == 1 && nControlledPairingCount == 1,
			QStringLiteral("Trusted peers requested pairing again"))
		|| !Require(nControllerAuthenticated == 2 && nControlledAuthenticated == 2,
			QStringLiteral("Trusted peers did not authenticate automatically: controller=%1 controlled=%2 controllerReason=%3 controlledReason=%4")
				.arg(nControllerAuthenticated).arg(nControlledAuthenticated,
					0, 10).arg(strControllerRejected, strControlledRejected)))
	{
		return 1;
	}
	return 0;
}
