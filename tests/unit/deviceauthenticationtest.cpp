#include "fakesecurity.h"
#include "core/security/tlspairingverification.h"
#include "session/deviceauthenticationflow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTextStream>
#include <QtCore/QUuid>

namespace
{
	bool Require(bool bCondition, const QString &strMessage)
	{
		if (bCondition)
			return true;
		QTextStream(stderr) << strMessage << Qt::endl;
		return false;
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
			[&strRejectedReason](const QString &strReason)
			{ strRejectedReason = strReason; });
		QString strError;
		if (!flow.beginIncoming(QStringLiteral("192.0.2.10"), 1,
			QStringLiteral("local"), &strError))
		{
			return Require(false, strError);
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
		if (!flow.beginIncoming(QStringLiteral("192.0.2.10"), 2,
			QStringLiteral("local"), &strError))
		{
			return Require(false, strError);
		}
		hello.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		hello.strVerificationMethod = KTlsPairingVerification::verificationMethod();
		flow.handleMessage(hello, 2);
		return Require(strRejectedReason
			== QStringLiteral("channel_binding_unavailable"),
			QStringLiteral("TLS exporter failure did not close pairing safely"));
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	if (!TestVerificationMethodAndExporterFailure())
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
	QObject::connect(&controller, &KDeviceAuthenticationFlow::messageReady,
		[&controlled, &nCurrentGeneration](const KTlsPairingMessage &message)
		{ controlled.handleMessage(message, nCurrentGeneration); });
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
		[&](const QString &strReason) { strControllerRejected = strReason; });
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::authenticationRejected,
		[&](const QString &strReason) { strControlledRejected = strReason; });

	QString strError;
	if (!Require(controlled.beginIncoming(QStringLiteral("192.0.2.1"),
		kGeneration, QStringLiteral("controlled"), &strError), strError))
	{
		return 1;
	}
	const QString strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const KPermissionScopes allPermissions = KPermissionScopes::fromInt(kAllPermissionScopeBits);
	if (!Require(controller.beginOutgoing(strRequestId, kGeneration,
		QStringLiteral("controller"), allPermissions, &strError), strError)
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
	controller.respondPairing(strControllerRequestId, true, allPermissions);
	controlled.respondPairing(strControlledRequestId, true, allPermissions);
	if (!Require(nControllerAuthenticated == 1 && nControlledAuthenticated == 1,
		QStringLiteral("Paired peers did not authenticate"))
		|| !Require(controller.context().effectivePermissions == allPermissions,
			QStringLiteral("Negotiated permissions are incorrect")))
	{
		return 1;
	}

	nCurrentGeneration = kGeneration + 1;
	if (!Require(controlled.beginIncoming(QStringLiteral("192.0.2.1"),
		kGeneration + 1, QStringLiteral("controlled"), &strError), strError)
		|| !Require(controller.beginOutgoing(
			QUuid::createUuid().toString(QUuid::WithoutBraces), kGeneration + 1,
			QStringLiteral("controller"), allPermissions, &strError), strError)
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
