#include "adapters/windows/security/atomicjsonfile.h"
#include "adapters/windows/security/certificatevaliditypolicy.h"
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

#include <windows.h>

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
		device.commitState = MutualTrustedDeviceCommitState;
		device.strPairingTransactionId = QUuid::createUuid().toString(
			QUuid::WithoutBraces);
		return device;
	}

	FILETIME FileTimeFromDateTime(const QDateTime &dateTime)
	{
		const QDateTime utc = dateTime.toUTC();
		const QDate date = utc.date();
		const QTime time = utc.time();
		SYSTEMTIME systemTime = {};
		systemTime.wYear = static_cast<WORD>(date.year());
		systemTime.wMonth = static_cast<WORD>(date.month());
		systemTime.wDay = static_cast<WORD>(date.day());
		systemTime.wHour = static_cast<WORD>(time.hour());
		systemTime.wMinute = static_cast<WORD>(time.minute());
		systemTime.wSecond = static_cast<WORD>(time.second());
		systemTime.wMilliseconds = static_cast<WORD>(time.msec());
		FILETIME fileTime = {};
		SystemTimeToFileTime(&systemTime, &fileTime);
		return fileTime;
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	const QDateTime policyNow(QDate(2026, 8, 17), QTime(12, 34, 56), Qt::UTC);
	const KCertificateValidityPeriod normalValidity =
		BuildDeviceCertificateValidityPeriod(policyNow);
	if (!Require(normalValidity.validFromUtc == policyNow.addSecs(-5 * 60),
		QStringLiteral("Certificate clock-skew tolerance is incorrect"))
		|| !Require(normalValidity.validToUtc == policyNow.addYears(5),
			QStringLiteral("Certificate lifetime is incorrect")))
	{
		return 1;
	}
	const QDateTime leapDay(QDate(2028, 2, 29), QTime(8, 0), Qt::UTC);
	const KCertificateValidityPeriod leapValidity =
		BuildDeviceCertificateValidityPeriod(leapDay);
	if (!Require(leapValidity.validToUtc.date() == QDate(2033, 2, 28),
		QStringLiteral("Leap-day certificate expiry is invalid")))
	{
		return 1;
	}

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
	const QDateTime creationStartedUtc = QDateTime::currentDateTimeUtc();
	if (!Require(provider.initialize(&strError), strError))
		return 1;
	const QDateTime creationFinishedUtc = QDateTime::currentDateTimeUtc();
	const KDeviceIdentity firstIdentity = provider.identity();
	if (!Require(firstIdentity.isValid(), QStringLiteral("Generated identity is invalid")))
		return 1;
	const KDeviceCertificate firstCertificate = provider.certificate();
	if (!Require(firstCertificate.validFromUtc
			>= creationStartedUtc.addSecs(-5 * 60 - 1)
			&& firstCertificate.validFromUtc
			<= creationFinishedUtc.addSecs(-5 * 60 + 1),
		QStringLiteral("Generated certificate was not backdated safely"))
		|| !Require(firstCertificate.validToUtc
			>= creationStartedUtc.addYears(5).addSecs(-1)
			&& firstCertificate.validToUtc
			<= creationFinishedUtc.addYears(5).addSecs(1),
			QStringLiteral("Generated certificate expiry is incorrect")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	PCCERT_CONTEXT pValidityCertificate = static_cast<PCCERT_CONTEXT>(
		provider.duplicateNativeCertificate(&strError));
	if (!Require(pValidityCertificate != nullptr, strError))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	FILETIME insideValidity = FileTimeFromDateTime(
		firstCertificate.validFromUtc.addSecs(1));
	FILETIME beforeValidity = FileTimeFromDateTime(
		firstCertificate.validFromUtc.addSecs(-1));
	FILETIME afterValidity = FileTimeFromDateTime(
		firstCertificate.validToUtc.addSecs(1));
	const bool bValidityCorrect = Require(CertVerifyTimeValidity(
		&insideValidity, pValidityCertificate->pCertInfo) == 0,
		QStringLiteral("Backdated certificate was not valid inside its window"))
		&& Require(CertVerifyTimeValidity(
			&beforeValidity, pValidityCertificate->pCertInfo) < 0,
			QStringLiteral("Certificate accepted time beyond clock-skew tolerance"))
		&& Require(CertVerifyTimeValidity(
			&afterValidity, pValidityCertificate->pCertInfo) > 0,
			QStringLiteral("Certificate accepted time after expiry"));
	CertFreeCertificateContext(pValidityCertificate);
	if (!bValidityCorrect)
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}

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

	PCCERT_CONTEXT pStoredCertificate = static_cast<PCCERT_CONTEXT>(
		provider.duplicateNativeCertificate(&strError));
	if (!Require(pStoredCertificate != nullptr, strError)
		|| !Require(CertDeleteCertificateFromStore(pStoredCertificate) != FALSE,
			QStringLiteral("Unable to remove certificate for renewal test")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	{
		KWindowsDeviceIdentityProvider renewed(strDirectoryPath);
		if (!Require(renewed.initialize(&strError), strError)
			|| !Require(renewed.identity().strDeviceId == firstIdentity.strDeviceId,
				QStringLiteral("Certificate renewal changed device identity"))
			|| !Require(renewed.certificate().spkiSha256
				== firstCertificate.spkiSha256,
				QStringLiteral("Certificate renewal changed the SPKI fingerprint")))
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

	QFile originalStoreFile(strStorePath);
	if (!Require(originalStoreFile.open(QIODevice::ReadOnly),
		QStringLiteral("Unable to read original trust store")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	const QByteArray originalStoreData = originalStoreFile.readAll();
	originalStoreFile.close();
	const HANDLE hLockedStore = CreateFileW(
		reinterpret_cast<LPCWSTR>(strStorePath.utf16()),
		GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (!Require(hLockedStore != INVALID_HANDLE_VALUE,
		QStringLiteral("Unable to lock trust store for atomic write test")))
	{
		provider.deletePersistedKey(nullptr);
		return 1;
	}
	strError.clear();
	const bool bLockedWriteSucceeded = KAtomicJsonFile::write(strStorePath,
		QByteArray("replacement-must-not-be-written"), &strError);
	CloseHandle(hLockedStore);
	QFile preservedStoreFile(strStorePath);
	const bool bPreservedStoreOpened = preservedStoreFile.open(QIODevice::ReadOnly);
	const QByteArray preservedStoreData = bPreservedStoreOpened
		? preservedStoreFile.readAll() : QByteArray();
	preservedStoreFile.close();
	if (!Require(!bLockedWriteSucceeded,
		QStringLiteral("Atomic write unexpectedly replaced a locked trust store"))
		|| !Require(!strError.isEmpty(),
			QStringLiteral("Atomic write failure did not report an error"))
		|| !Require(bPreservedStoreOpened && preservedStoreData == originalStoreData,
			QStringLiteral("Failed atomic replacement damaged the original trust store")))
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
