#include "fakesecurity.h"
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
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	KFakeDeviceIdentityProvider controllerIdentity;
	KFakeDeviceIdentityProvider controlledIdentity;
	KFakeTrustedDeviceStore controllerStore;
	KFakeTrustedDeviceStore controlledStore;
	controllerStore.setIdentityProvider(&controllerIdentity);
	controlledStore.setIdentityProvider(&controlledIdentity);
	KDeviceAuthenticationFlow controller(&controllerIdentity, &controllerStore);
	KDeviceAuthenticationFlow controlled(&controlledIdentity, &controlledStore);
	constexpr quint64 kGeneration = 7;
	quint64 nCurrentGeneration = kGeneration;
	QObject::connect(&controller, &KDeviceAuthenticationFlow::messageReady,
		[&controlled, &nCurrentGeneration](const KIdentityMessage &message)
		{ controlled.handleMessage(message, nCurrentGeneration); });
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::messageReady,
		[&controller, &nCurrentGeneration](const KIdentityMessage &message)
		{ controller.handleMessage(message, nCurrentGeneration); });

	QString strControllerCode;
	QString strControlledCode;
	QString strControllerRequestId;
	QString strControlledRequestId;
	int nControllerPairingCount = 0;
	int nControlledPairingCount = 0;
	int nControllerAuthenticated = 0;
	int nControlledAuthenticated = 0;
	QString strControllerRejected;
	QString strControlledRejected;
	QObject::connect(&controller, &KDeviceAuthenticationFlow::pairingRequested,
		[&](const QString &strRequestId, const QString &, const QString &,
			const QString &strCode, KPermissionScopes, qint64)
		{
			strControllerRequestId = strRequestId;
			strControllerCode = strCode;
			++nControllerPairingCount;
		});
	QObject::connect(&controlled, &KDeviceAuthenticationFlow::pairingRequested,
		[&](const QString &strRequestId, const QString &, const QString &,
			const QString &strCode, KPermissionScopes, qint64)
		{
			strControlledRequestId = strRequestId;
			strControlledCode = strCode;
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
		|| !Require(strControllerCode == strControlledCode && strControllerCode.size() == 6,
			QStringLiteral("Pairing codes do not match")))
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
