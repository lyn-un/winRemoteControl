#include "adapters/windows/security/signedjsontrusteddevicestore.h"
#include "adapters/windows/security/windowsdeviceidentityprovider.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
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

	KTrustedDevice MakeTrustedDevice(const KDeviceIdentity &identity,
		const KDeviceCertificate &certificate)
	{
		KTrustedDevice device;
		device.strDeviceId = identity.strDeviceId;
		device.spkiSha256 = certificate.spkiSha256;
		device.certificateSha256 = certificate.certificateSha256;
		device.strFingerprint = certificate.spkiFingerprint();
		device.strAlias = QStringLiteral("Test device");
		device.strAdvertisedName = QStringLiteral("Test host");
		device.permissionLimit = KPermissionScopes(
			ViewScreenPermissionScope | InputControlPermissionScope);
		device.nPairedAtMs = 100;
		device.nLastAuthenticatedAtMs = 200;
		return device;
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	const QString strDirectoryPath = QDir(QCoreApplication::applicationDirPath())
		.filePath(QStringLiteral("device-identity-test-%1")
			.arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
	QDir directory(strDirectoryPath);
	if (!Require(QDir().mkpath(strDirectoryPath),
		QStringLiteral("Temporary directory is unavailable")))
		return 1;
	QFile probeFile(directory.filePath(QStringLiteral("probe.txt")));
	if (!Require(probeFile.open(QIODevice::WriteOnly),
		QStringLiteral("Probe file is unavailable: %1").arg(probeFile.errorString())))
		return 1;
	probeFile.write("probe");
	probeFile.close();

	QString strError;
	KWindowsDeviceIdentityProvider provider(strDirectoryPath);
	if (!Require(provider.initialize(&strError), strError))
		return 1;
	const KDeviceIdentity firstIdentity = provider.identity();
	if (!Require(firstIdentity.isValid(), QStringLiteral("Generated identity is invalid")))
		return 1;

	const QByteArray payload("identity-test-payload");
	QByteArray signature;
	if (!Require(provider.sign(payload, &signature, &strError), strError)
		|| !Require(signature.size() == 64, QStringLiteral("Unexpected signature size"))
		|| !Require(provider.verify(firstIdentity.publicKey, payload, signature, &strError), strError)
		|| !Require(!provider.verify(firstIdentity.publicKey,
			payload + 'x', signature, nullptr), QStringLiteral("Tampered payload was accepted")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}

	{
		KWindowsDeviceIdentityProvider reopened(strDirectoryPath);
		if (!Require(reopened.initialize(&strError), strError)
			|| !Require(reopened.identity().strDeviceId == firstIdentity.strDeviceId,
				QStringLiteral("Identity did not persist")))
		{
			provider.deletePersistedKey(nullptr);
			return 1;
		}
	}

	const QString strStorePath = directory.filePath(QStringLiteral("trusted_devices.json"));
	KSignedJsonTrustedDeviceStore store(strStorePath);
	store.setIdentityProvider(&provider);
	const QVector<KTrustedDevice> savedDevices = {
		MakeTrustedDevice(firstIdentity, provider.certificate())
	};
	if (!Require(store.saveDevices(savedDevices, &strError), strError))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	const QVector<KTrustedDevice> loadedDevices = store.loadDevices(&strError);
	if (!Require(loadedDevices.size() == 1,
		QStringLiteral("Trusted device store did not round trip")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}

	QFile storeFile(strStorePath);
	if (!Require(storeFile.open(QIODevice::Append), QStringLiteral("Unable to tamper store")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	storeFile.write("x");
	storeFile.close();
	strError.clear();
	store.loadDevices(&strError);
	if (!Require(strError.startsWith(QStringLiteral("trust_store_tampered")),
		QStringLiteral("Tampered store was not rejected")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}

	QJsonObject legacyStore;
	legacyStore.insert(QStringLiteral("version"), 1);
	legacyStore.insert(QStringLiteral("devices"), QJsonArray());
	legacyStore.insert(QStringLiteral("signature"), QStringLiteral("legacy"));
	if (!Require(storeFile.open(QIODevice::WriteOnly | QIODevice::Truncate),
		QStringLiteral("Unable to create legacy trust store")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	storeFile.write(QJsonDocument(legacyStore).toJson(QJsonDocument::Compact));
	storeFile.close();
	strError.clear();
	if (!Require(store.loadDevices(&strError).isEmpty() && strError.isEmpty(),
		QStringLiteral("Legacy trust store was not invalidated safely"))
		|| !Require(QFile::exists(strStorePath + QStringLiteral(".v1.bak")),
			QStringLiteral("Legacy trust store backup was not created"))
		|| !Require(!store.takeMigrationNotice().isEmpty(),
			QStringLiteral("Legacy trust migration notice was not published"))
		|| !Require(store.takeMigrationNotice().isEmpty(),
			QStringLiteral("Legacy trust migration notice was not one-shot")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}

	if (!Require(provider.deletePersistedKey(&strError), strError))
		return 1;
	KWindowsDeviceIdentityProvider copiedDescriptor(strDirectoryPath);
	if (!Require(copiedDescriptor.initialize(&strError), strError)
		|| !Require(copiedDescriptor.identity().strDeviceId != firstIdentity.strDeviceId,
			QStringLiteral("Copied descriptor reused an unavailable private key")))
	{
		copiedDescriptor.deletePersistedKey(nullptr);
		return 1;
	}
	copiedDescriptor.deletePersistedKey(nullptr);
	directory.removeRecursively();
	return 0;
}
