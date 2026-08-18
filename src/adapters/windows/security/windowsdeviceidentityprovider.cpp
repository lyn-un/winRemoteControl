#include "adapters/windows/security/windowsdeviceidentityprovider.h"

#include "adapters/windows/security/atomicjsonfile.h"
#include "adapters/windows/security/certificatevaliditypolicy.h"
#include "core/security/securitycanonicalwriter.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>

namespace
{
	constexpr int kIdentityFormatVersion = 1;
	constexpr int kP256PublicKeyBytes = 65;
	constexpr int kP256SignatureBytes = 64;
	constexpr int kSha256Bytes = 32;
	constexpr int kCertificateRenewalDays = 30;

	QString StatusMessage(const QString &strOperation, SECURITY_STATUS status)
	{
		return QStringLiteral("%1 failed (0x%2)")
			.arg(strOperation)
			.arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
	}

	QString DescriptorPath(const QString &strDirectory)
	{
		return QDir(strDirectory).filePath(QStringLiteral("device_identity.json"));
	}

	QString KeyName(const QString &strDeviceId)
	{
		return QStringLiteral("winRemoteControl.device.%1")
			.arg(QUuid::fromString(strDeviceId).toString(QUuid::WithoutBraces));
	}

	void DeleteCertificatesForDevice(const QString &strDeviceId)
	{
		if (QUuid(strDeviceId).isNull())
			return;
		HCERTSTORE hStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
			CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
		if (hStore == nullptr)
			return;
		const QString strSubject = QStringLiteral("winRemoteControl Device %1")
			.arg(strDeviceId);
		for (;;)
		{
			PCCERT_CONTEXT pCertificate = CertFindCertificateInStore(hStore,
				X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W,
				reinterpret_cast<LPCWSTR>(strSubject.utf16()), nullptr);
			if (pCertificate == nullptr)
				break;
			if (!CertDeleteCertificateFromStore(pCertificate))
				break;
		}
		CertCloseStore(hStore, 0);
	}

	QByteArray DecodeBase64Url(const QString &strValue)
	{
		return QByteArray::fromBase64(strValue.toLatin1(),
			QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
	}

	QString EncodeBase64Url(const QByteArray &value)
	{
		return QString::fromLatin1(value.toBase64(
			QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	}

	QByteArray IdentityDescriptorData(const KDeviceIdentity &identity)
	{
		KSecurityCanonicalWriter writer;
		writer.appendString(QStringLiteral("wrc.device-identity"));
		writer.appendUInt32(kIdentityFormatVersion);
		writer.appendString(identity.strDeviceId);
		writer.appendString(identity.strAlgorithm);
		writer.appendBytes(identity.publicKey);
		return writer.data();
	}

	bool Sha256(const QByteArray &data, QByteArray *pDigest, QString *pErrorMessage)
	{
		BCRYPT_ALG_HANDLE hAlgorithm = nullptr;
		NTSTATUS status = BCryptOpenAlgorithmProvider(
			&hAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (status < 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = StatusMessage(QStringLiteral("BCryptOpenAlgorithmProvider"), status);
			return false;
		}

		QByteArray digest(kSha256Bytes, Qt::Uninitialized);
		status = BCryptHash(hAlgorithm, nullptr, 0,
			reinterpret_cast<PUCHAR>(const_cast<char *>(data.constData())),
			static_cast<ULONG>(data.size()),
			reinterpret_cast<PUCHAR>(digest.data()),
			static_cast<ULONG>(digest.size()));
		BCryptCloseAlgorithmProvider(hAlgorithm, 0);
		if (status < 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = StatusMessage(QStringLiteral("BCryptHash"), status);
			return false;
		}
		*pDigest = digest;
		return true;
	}

	QDateTime DateTimeFromFileTime(const FILETIME &fileTime)
	{
		SYSTEMTIME systemTime = {};
		if (!FileTimeToSystemTime(&fileTime, &systemTime))
			return QDateTime();
		return QDateTime(QDate(systemTime.wYear, systemTime.wMonth, systemTime.wDay),
			QTime(systemTime.wHour, systemTime.wMinute, systemTime.wSecond,
				systemTime.wMilliseconds), Qt::UTC);
	}

	bool SystemTimeFromDateTime(const QDateTime &dateTime, SYSTEMTIME *pSystemTime)
	{
		if (pSystemTime == nullptr || !dateTime.isValid())
			return false;
		const QDateTime utc = dateTime.toUTC();
		const QDate date = utc.date();
		const QTime time = utc.time();
		*pSystemTime = {};
		pSystemTime->wYear = static_cast<WORD>(date.year());
		pSystemTime->wMonth = static_cast<WORD>(date.month());
		pSystemTime->wDay = static_cast<WORD>(date.day());
		pSystemTime->wHour = static_cast<WORD>(time.hour());
		pSystemTime->wMinute = static_cast<WORD>(time.minute());
		pSystemTime->wSecond = static_cast<WORD>(time.second());
		pSystemTime->wMilliseconds = static_cast<WORD>(time.msec());
		return true;
	}

	bool EncodeCertificateObject(LPCSTR pszType, const void *pValue,
		QByteArray *pEncoded, QString *pErrorMessage)
	{
		DWORD nBytes = 0;
		if (!CryptEncodeObjectEx(X509_ASN_ENCODING, pszType, pValue, 0,
			nullptr, nullptr, &nBytes))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("CryptEncodeObjectEx(size) failed (%1)")
					.arg(GetLastError());
			return false;
		}
		QByteArray encoded(static_cast<int>(nBytes), Qt::Uninitialized);
		if (!CryptEncodeObjectEx(X509_ASN_ENCODING, pszType, pValue, 0,
			nullptr, encoded.data(), &nBytes))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("CryptEncodeObjectEx failed (%1)")
					.arg(GetLastError());
			return false;
		}
		encoded.resize(static_cast<int>(nBytes));
		*pEncoded = encoded;
		return true;
	}

	bool CertificateData(PCCERT_CONTEXT pCertificate,
		const QString &strDeviceId,
		KDeviceCertificate *pResult,
		QString *pErrorMessage)
	{
		if (pCertificate == nullptr || pResult == nullptr)
			return false;
		QByteArray spkiDer;
		if (!EncodeCertificateObject(X509_PUBLIC_KEY_INFO,
			&pCertificate->pCertInfo->SubjectPublicKeyInfo, &spkiDer, pErrorMessage))
		{
			return false;
		}
		KDeviceCertificate result;
		result.strDeviceId = strDeviceId;
		result.certificateDer = QByteArray(
			reinterpret_cast<const char *>(pCertificate->pbCertEncoded),
			static_cast<int>(pCertificate->cbCertEncoded));
		result.spkiSha256 = QCryptographicHash::hash(spkiDer, QCryptographicHash::Sha256);
		result.certificateSha256 = QCryptographicHash::hash(
			result.certificateDer, QCryptographicHash::Sha256);
		result.validFromUtc = DateTimeFromFileTime(pCertificate->pCertInfo->NotBefore);
		result.validToUtc = DateTimeFromFileTime(pCertificate->pCertInfo->NotAfter);
		if (!result.isValid())
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Generated device certificate is invalid");
			return false;
		}
		*pResult = result;
		return true;
	}

	bool CertificateMatchesPublicKey(PCCERT_CONTEXT pCertificate,
		const QByteArray &publicKey)
	{
		if (pCertificate == nullptr || publicKey.size() != kP256PublicKeyBytes
			|| publicKey.at(0) != '\x04')
		{
			return false;
		}
		BCRYPT_KEY_HANDLE hPublicKey = nullptr;
		if (!CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING,
			&pCertificate->pCertInfo->SubjectPublicKeyInfo, 0, nullptr, &hPublicKey))
		{
			return false;
		}
		DWORD nBlobBytes = 0;
		NTSTATUS status = BCryptExportKey(hPublicKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
			nullptr, 0, &nBlobBytes, 0);
		QByteArray blob(static_cast<int>(nBlobBytes), Qt::Uninitialized);
		if (status >= 0)
		{
			status = BCryptExportKey(hPublicKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
				reinterpret_cast<PUCHAR>(blob.data()), nBlobBytes, &nBlobBytes, 0);
		}
		BCryptDestroyKey(hPublicKey);
		if (status < 0 || blob.size() != static_cast<int>(sizeof(BCRYPT_ECCKEY_BLOB)) + 64)
			return false;
		const auto *pHeader = reinterpret_cast<const BCRYPT_ECCKEY_BLOB *>(blob.constData());
		return pHeader->cbKey == 32
			&& memcmp(blob.constData() + sizeof(BCRYPT_ECCKEY_BLOB),
				publicKey.constData() + 1, 64) == 0;
	}
}

KWindowsDeviceIdentityProvider::KWindowsDeviceIdentityProvider(
	const QString &strSecurityDirectory)
	: m_strSecurityDirectory(strSecurityDirectory)
{
}

KWindowsDeviceIdentityProvider::~KWindowsDeviceIdentityProvider()
{
	closeCertificate();
	closeKey();
	if (m_hProvider != 0)
		NCryptFreeObject(m_hProvider);
}

bool KWindowsDeviceIdentityProvider::initialize(QString *pErrorMessage)
{
	if (m_hKey != 0 && m_identity.isValid() && m_certificate.isValid())
		return true;
	if (m_hKey != 0 && m_identity.isValid())
		return ensureCertificate(pErrorMessage);
	if (!QDir().mkpath(m_strSecurityDirectory))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to create the security directory");
		return false;
	}
	if (m_hProvider == 0)
	{
		const SECURITY_STATUS status = NCryptOpenStorageProvider(
			&m_hProvider, MS_KEY_STORAGE_PROVIDER, 0);
		if (status != ERROR_SUCCESS)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = StatusMessage(QStringLiteral("NCryptOpenStorageProvider"), status);
			return false;
		}
	}
	const bool bIdentityReady = QFile::exists(DescriptorPath(m_strSecurityDirectory))
		? loadDescriptor(pErrorMessage) : createIdentity(pErrorMessage);
	return bIdentityReady && ensureCertificate(pErrorMessage);
}

KDeviceIdentity KWindowsDeviceIdentityProvider::identity() const
{
	return m_identity;
}

bool KWindowsDeviceIdentityProvider::sign(const QByteArray &data,
	QByteArray *pSignature,
	QString *pErrorMessage) const
{
	if (m_hKey == 0 || pSignature == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Device identity is not initialized");
		return false;
	}

	QByteArray digest;
	if (!Sha256(data, &digest, pErrorMessage))
		return false;
	DWORD nSignatureBytes = 0;
	SECURITY_STATUS status = NCryptSignHash(m_hKey, nullptr,
		reinterpret_cast<PBYTE>(digest.data()), static_cast<DWORD>(digest.size()),
		nullptr, 0, &nSignatureBytes, 0);
	if (status != ERROR_SUCCESS || nSignatureBytes != kP256SignatureBytes)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = StatusMessage(QStringLiteral("NCryptSignHash(size)"), status);
		return false;
	}
	QByteArray signature(static_cast<int>(nSignatureBytes), Qt::Uninitialized);
	status = NCryptSignHash(m_hKey, nullptr,
		reinterpret_cast<PBYTE>(digest.data()), static_cast<DWORD>(digest.size()),
		reinterpret_cast<PBYTE>(signature.data()), nSignatureBytes,
		&nSignatureBytes, 0);
	if (status != ERROR_SUCCESS)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = StatusMessage(QStringLiteral("NCryptSignHash"), status);
		return false;
	}
	*pSignature = signature;
	return true;
}

bool KWindowsDeviceIdentityProvider::verify(const QByteArray &publicKey,
	const QByteArray &data,
	const QByteArray &signature,
	QString *pErrorMessage) const
{
	if (publicKey.size() != kP256PublicKeyBytes || publicKey.at(0) != '\x04'
		|| signature.size() != kP256SignatureBytes)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid ECDSA public key or signature size");
		return false;
	}

	QByteArray digest;
	if (!Sha256(data, &digest, pErrorMessage))
		return false;
	BCRYPT_ALG_HANDLE hAlgorithm = nullptr;
	NTSTATUS status = BCryptOpenAlgorithmProvider(
		&hAlgorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
	if (status < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = StatusMessage(QStringLiteral("BCryptOpenAlgorithmProvider"), status);
		return false;
	}

	QByteArray blob(static_cast<int>(sizeof(BCRYPT_ECCKEY_BLOB)) + 64, Qt::Uninitialized);
	auto *pHeader = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
	pHeader->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
	pHeader->cbKey = 32;
	memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), publicKey.constData() + 1, 64);
	BCRYPT_KEY_HANDLE hPublicKey = nullptr;
	status = BCryptImportKeyPair(hAlgorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB,
		&hPublicKey, reinterpret_cast<PUCHAR>(blob.data()),
		static_cast<ULONG>(blob.size()), 0);
	if (status >= 0)
	{
		status = BCryptVerifySignature(hPublicKey, nullptr,
			reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()),
			reinterpret_cast<PUCHAR>(const_cast<char *>(signature.constData())),
			static_cast<ULONG>(signature.size()), 0);
	}
	if (hPublicKey != nullptr)
		BCryptDestroyKey(hPublicKey);
	BCryptCloseAlgorithmProvider(hAlgorithm, 0);
	if (status >= 0)
		return true;
	if (pErrorMessage != nullptr)
		*pErrorMessage = StatusMessage(QStringLiteral("BCryptVerifySignature"), status);
	return false;
}

QByteArray KWindowsDeviceIdentityProvider::randomBytes(int nByteCount,
	QString *pErrorMessage) const
{
	if (nByteCount <= 0)
		return QByteArray();
	QByteArray bytes(nByteCount, Qt::Uninitialized);
	const NTSTATUS status = BCryptGenRandom(nullptr,
		reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (status >= 0)
		return bytes;
	if (pErrorMessage != nullptr)
		*pErrorMessage = StatusMessage(QStringLiteral("BCryptGenRandom"), status);
	return QByteArray();
}

KDeviceCertificate KWindowsDeviceIdentityProvider::certificate() const
{
	return m_certificate;
}

void *KWindowsDeviceIdentityProvider::duplicateNativeCertificate(
	QString *pErrorMessage) const
{
	if (m_pCertificateContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Device certificate is not initialized");
		return nullptr;
	}
	PCCERT_CONTEXT pDuplicate = CertDuplicateCertificateContext(
		static_cast<PCCERT_CONTEXT>(m_pCertificateContext));
	if (pDuplicate == nullptr && pErrorMessage != nullptr)
	{
		*pErrorMessage = QStringLiteral("CertDuplicateCertificateContext failed (%1)")
			.arg(GetLastError());
	}
	return const_cast<PCERT_CONTEXT>(pDuplicate);
}

bool KWindowsDeviceIdentityProvider::deletePersistedKey(QString *pErrorMessage)
{
	const QString strDeviceId = m_identity.strDeviceId;
	closeCertificate();
	DeleteCertificatesForDevice(strDeviceId);
	if (m_hKey == 0)
	{
		m_identity = KDeviceIdentity();
		return true;
	}
	const SECURITY_STATUS status = NCryptDeleteKey(m_hKey, 0);
	m_hKey = 0;
	m_identity = KDeviceIdentity();
	if (status == ERROR_SUCCESS)
		return true;
	if (pErrorMessage != nullptr)
		*pErrorMessage = StatusMessage(QStringLiteral("NCryptDeleteKey"), status);
	return false;
}

bool KWindowsDeviceIdentityProvider::loadDescriptor(QString *pErrorMessage)
{
	QFile file(DescriptorPath(m_strSecurityDirectory));
	if (!file.open(QIODevice::ReadOnly))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to open device identity descriptor");
		return false;
	}
	QJsonParseError parseError;
	const QByteArray descriptorData = file.readAll();
	file.close();
	const QJsonDocument document = QJsonDocument::fromJson(descriptorData, &parseError);
	const QJsonObject object = document.object();
	KDeviceIdentity loaded;
	loaded.strDeviceId = object.value(QStringLiteral("deviceId")).toString();
	loaded.strAlgorithm = object.value(QStringLiteral("algorithm")).toString();
	loaded.publicKey = DecodeBase64Url(object.value(QStringLiteral("publicKey")).toString());
	loaded.strFingerprint = DevicePublicKeyFingerprint(loaded.publicKey);
	const QByteArray signature = DecodeBase64Url(object.value(QStringLiteral("signature")).toString());
	if (parseError.error != QJsonParseError::NoError
		|| object.value(QStringLiteral("version")).toInt() != kIdentityFormatVersion
		|| !loaded.isValid()
		|| signature.size() != kP256SignatureBytes)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid device identity descriptor");
		return false;
	}

	bool bMissing = false;
	if (!openPersistedKey(loaded.strDeviceId, &bMissing, pErrorMessage))
	{
		if (bMissing)
			return createIdentity(pErrorMessage);
		return false;
	}
	QByteArray exportedPublicKey;
	if (!exportPublicKey(&exportedPublicKey, pErrorMessage)
		|| exportedPublicKey != loaded.publicKey
		|| !verify(loaded.publicKey, IdentityDescriptorData(loaded), signature, pErrorMessage))
	{
		closeKey();
		if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
			*pErrorMessage = QStringLiteral("Device identity descriptor verification failed");
		return false;
	}
	m_identity = loaded;
	return true;
}

bool KWindowsDeviceIdentityProvider::createIdentity(QString *pErrorMessage)
{
	closeKey();
	KDeviceIdentity created;
	created.strDeviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	created.strAlgorithm = QStringLiteral("ecdsa-p256-sha256");
	const QString strKeyName = KeyName(created.strDeviceId);
	QString strFailedOperation = QStringLiteral("NCryptCreatePersistedKey");
	SECURITY_STATUS status = NCryptCreatePersistedKey(m_hProvider, &m_hKey,
		NCRYPT_ECDSA_P256_ALGORITHM, reinterpret_cast<LPCWSTR>(strKeyName.utf16()), 0, 0);
	if (status == ERROR_SUCCESS)
	{
		DWORD nExportPolicy = 0;
		strFailedOperation = QStringLiteral("NCryptSetProperty(export policy)");
		status = NCryptSetProperty(m_hKey, NCRYPT_EXPORT_POLICY_PROPERTY,
			reinterpret_cast<PBYTE>(&nExportPolicy), sizeof(nExportPolicy), 0);
	}
	if (status == ERROR_SUCCESS)
	{
		strFailedOperation = QStringLiteral("NCryptFinalizeKey");
		status = NCryptFinalizeKey(m_hKey, 0);
	}
	DWORD nExportPolicy = 0;
	DWORD nPropertyBytes = 0;
	if (status == ERROR_SUCCESS)
	{
		strFailedOperation = QStringLiteral("NCryptGetProperty(export policy)");
		status = NCryptGetProperty(m_hKey, NCRYPT_EXPORT_POLICY_PROPERTY,
			reinterpret_cast<PBYTE>(&nExportPolicy), sizeof(nExportPolicy),
			&nPropertyBytes, 0);
		if (status == ERROR_SUCCESS && nExportPolicy != 0)
			status = NTE_PERM;
	}
	if (status != ERROR_SUCCESS)
	{
		closeKey();
		if (pErrorMessage != nullptr)
			*pErrorMessage = StatusMessage(strFailedOperation, status);
		return false;
	}
	if (!exportPublicKey(&created.publicKey, pErrorMessage))
	{
		deletePersistedKey(nullptr);
		return false;
	}
	created.strFingerprint = DevicePublicKeyFingerprint(created.publicKey);
	m_identity = created;
	if (saveDescriptor(pErrorMessage))
		return true;
	deletePersistedKey(nullptr);
	return false;
}

bool KWindowsDeviceIdentityProvider::openPersistedKey(const QString &strDeviceId,
	bool *pMissing,
	QString *pErrorMessage)
{
	closeKey();
	if (pMissing != nullptr)
		*pMissing = false;
	const QString strKeyName = KeyName(strDeviceId);
	const SECURITY_STATUS status = NCryptOpenKey(m_hProvider, &m_hKey,
		reinterpret_cast<LPCWSTR>(strKeyName.utf16()), 0, 0);
	if (status == ERROR_SUCCESS)
		return true;
	if (status == NTE_BAD_KEYSET || status == NTE_NOT_FOUND)
	{
		if (pMissing != nullptr)
			*pMissing = true;
		return false;
	}
	if (pErrorMessage != nullptr)
		*pErrorMessage = StatusMessage(QStringLiteral("NCryptOpenKey"), status);
	return false;
}

bool KWindowsDeviceIdentityProvider::exportPublicKey(QByteArray *pPublicKey,
	QString *pErrorMessage) const
{
	DWORD nBlobBytes = 0;
	SECURITY_STATUS status = NCryptExportKey(m_hKey, 0, BCRYPT_ECCPUBLIC_BLOB,
		nullptr, nullptr, 0, &nBlobBytes, 0);
	QByteArray blob(static_cast<int>(nBlobBytes), Qt::Uninitialized);
	if (status == ERROR_SUCCESS)
	{
		status = NCryptExportKey(m_hKey, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr,
			reinterpret_cast<PBYTE>(blob.data()), nBlobBytes, &nBlobBytes, 0);
	}
	if (status != ERROR_SUCCESS
		|| blob.size() != static_cast<int>(sizeof(BCRYPT_ECCKEY_BLOB)) + 64)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = StatusMessage(QStringLiteral("NCryptExportKey"), status);
		return false;
	}
	const auto *pHeader = reinterpret_cast<const BCRYPT_ECCKEY_BLOB *>(blob.constData());
	if (pHeader->dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || pHeader->cbKey != 32)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unexpected CNG public key format");
		return false;
	}
	QByteArray publicKey(kP256PublicKeyBytes, Qt::Uninitialized);
	publicKey[0] = '\x04';
	memcpy(publicKey.data() + 1, blob.constData() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
	*pPublicKey = publicKey;
	return true;
}

bool KWindowsDeviceIdentityProvider::saveDescriptor(QString *pErrorMessage) const
{
	QByteArray signature;
	if (!sign(IdentityDescriptorData(m_identity), &signature, pErrorMessage))
		return false;
	QJsonObject object;
	object.insert(QStringLiteral("version"), kIdentityFormatVersion);
	object.insert(QStringLiteral("deviceId"), m_identity.strDeviceId);
	object.insert(QStringLiteral("algorithm"), m_identity.strAlgorithm);
	object.insert(QStringLiteral("publicKey"), EncodeBase64Url(m_identity.publicKey));
	object.insert(QStringLiteral("signature"), EncodeBase64Url(signature));
	return KAtomicJsonFile::write(DescriptorPath(m_strSecurityDirectory),
		QJsonDocument(object).toJson(QJsonDocument::Indented), pErrorMessage);
}

bool KWindowsDeviceIdentityProvider::ensureCertificate(QString *pErrorMessage)
{
	closeCertificate();
	if (loadCertificate(nullptr)
		&& m_certificate.validToUtc > QDateTime::currentDateTimeUtc().addDays(kCertificateRenewalDays))
	{
		return true;
	}
	closeCertificate();
	return createCertificate(pErrorMessage);
}

bool KWindowsDeviceIdentityProvider::loadCertificate(QString *pErrorMessage)
{
	HCERTSTORE hStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
		CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
	if (hStore == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to open the current-user certificate store");
		return false;
	}
	const QString strSubject = QStringLiteral("winRemoteControl Device %1")
		.arg(m_identity.strDeviceId);
	PCCERT_CONTEXT pFound = nullptr;
	PCCERT_CONTEXT pCandidate = nullptr;
	QDateTime latestExpiry;
	while ((pCandidate = CertFindCertificateInStore(hStore,
		X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W,
		reinterpret_cast<LPCWSTR>(strSubject.utf16()), pCandidate)) != nullptr)
	{
		KDeviceCertificate candidate;
		if (CertificateMatchesPublicKey(pCandidate, m_identity.publicKey)
			&& CertificateData(pCandidate, m_identity.strDeviceId, &candidate, nullptr)
			&& (!pFound || candidate.validToUtc > latestExpiry))
		{
			if (pFound != nullptr)
				CertFreeCertificateContext(pFound);
			pFound = CertDuplicateCertificateContext(pCandidate);
			latestExpiry = candidate.validToUtc;
		}
	}
	CertCloseStore(hStore, 0);
	if (pFound == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Device certificate was not found");
		return false;
	}
	KDeviceCertificate certificate;
	if (!CertificateData(pFound, m_identity.strDeviceId, &certificate, pErrorMessage))
	{
		CertFreeCertificateContext(pFound);
		return false;
	}
	m_pCertificateContext = const_cast<PCERT_CONTEXT>(pFound);
	m_certificate = certificate;
	return true;
}

bool KWindowsDeviceIdentityProvider::createCertificate(QString *pErrorMessage)
{
	const QString strCommonName = QStringLiteral("winRemoteControl Device %1")
		.arg(m_identity.strDeviceId);
	const QString strSubject = QStringLiteral("CN=%1").arg(strCommonName);
	DWORD nSubjectBytes = 0;
	if (!CertStrToNameW(X509_ASN_ENCODING,
		reinterpret_cast<LPCWSTR>(strSubject.utf16()), CERT_X500_NAME_STR,
		nullptr, nullptr, &nSubjectBytes, nullptr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("CertStrToNameW(size) failed (%1)").arg(GetLastError());
		return false;
	}
	QByteArray subjectBytes(static_cast<int>(nSubjectBytes), Qt::Uninitialized);
	if (!CertStrToNameW(X509_ASN_ENCODING,
		reinterpret_cast<LPCWSTR>(strSubject.utf16()), CERT_X500_NAME_STR,
		nullptr, reinterpret_cast<BYTE *>(subjectBytes.data()), &nSubjectBytes, nullptr))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("CertStrToNameW failed (%1)").arg(GetLastError());
		return false;
	}
	CERT_NAME_BLOB subject = { nSubjectBytes,
		reinterpret_cast<BYTE *>(subjectBytes.data()) };

	BYTE nKeyUsage = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
	CRYPT_BIT_BLOB keyUsage = { 1, &nKeyUsage, 0 };
	QByteArray keyUsageDer;
	if (!EncodeCertificateObject(X509_KEY_USAGE, &keyUsage, &keyUsageDer, pErrorMessage))
		return false;
	LPSTR usages[] = {
		const_cast<LPSTR>(szOID_PKIX_KP_CLIENT_AUTH),
		const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)
	};
	CERT_ENHKEY_USAGE enhancedUsage = { 2, usages };
	QByteArray enhancedUsageDer;
	if (!EncodeCertificateObject(X509_ENHANCED_KEY_USAGE, &enhancedUsage,
		&enhancedUsageDer, pErrorMessage))
	{
		return false;
	}
	const QString strUri = QStringLiteral("urn:wrc:device:%1").arg(m_identity.strDeviceId);
	CERT_ALT_NAME_ENTRY alternateNameEntry = {};
	alternateNameEntry.dwAltNameChoice = CERT_ALT_NAME_URL;
	alternateNameEntry.pwszURL = const_cast<LPWSTR>(
		reinterpret_cast<LPCWSTR>(strUri.utf16()));
	CERT_ALT_NAME_INFO alternateNames = { 1, &alternateNameEntry };
	QByteArray alternateNamesDer;
	if (!EncodeCertificateObject(X509_ALTERNATE_NAME, &alternateNames,
		&alternateNamesDer, pErrorMessage))
	{
		return false;
	}
	CERT_EXTENSION extensions[] = {
		{ const_cast<LPSTR>(szOID_KEY_USAGE), TRUE,
			{ static_cast<DWORD>(keyUsageDer.size()),
				reinterpret_cast<BYTE *>(keyUsageDer.data()) } },
		{ const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), TRUE,
			{ static_cast<DWORD>(enhancedUsageDer.size()),
				reinterpret_cast<BYTE *>(enhancedUsageDer.data()) } },
		{ const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE,
			{ static_cast<DWORD>(alternateNamesDer.size()),
				reinterpret_cast<BYTE *>(alternateNamesDer.data()) } }
	};
	CERT_EXTENSIONS certificateExtensions = { 3, extensions };

	const KCertificateValidityPeriod validity =
		BuildDeviceCertificateValidityPeriod(QDateTime::currentDateTimeUtc());
	SYSTEMTIME validFrom = {};
	SYSTEMTIME validTo = {};
	if (!SystemTimeFromDateTime(validity.validFromUtc, &validFrom)
		|| !SystemTimeFromDateTime(validity.validToUtc, &validTo))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to calculate certificate validity period");
		return false;
	}
	CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm = {};
	signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_ECDSA_SHA256);
	const QString strKeyName = KeyName(m_identity.strDeviceId);
	CRYPT_KEY_PROV_INFO keyProvider = {};
	keyProvider.pwszContainerName = const_cast<LPWSTR>(
		reinterpret_cast<LPCWSTR>(strKeyName.utf16()));
	keyProvider.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
	keyProvider.dwProvType = 0;
	keyProvider.dwFlags = NCRYPT_SILENT_FLAG;
	keyProvider.dwKeySpec = 0;
	PCCERT_CONTEXT pCreated = CertCreateSelfSignCertificate(m_hKey, &subject, 0,
		&keyProvider, &signatureAlgorithm, &validFrom, &validTo, &certificateExtensions);
	if (pCreated == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("CertCreateSelfSignCertificate failed (%1)")
				.arg(GetLastError());
		return false;
	}

	HCERTSTORE hStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
		CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
	PCCERT_CONTEXT pStored = nullptr;
	const bool bStored = hStore != nullptr
		&& CertAddCertificateContextToStore(hStore, pCreated,
			CERT_STORE_ADD_REPLACE_EXISTING, &pStored);
	if (hStore != nullptr)
		CertCloseStore(hStore, 0);
	CertFreeCertificateContext(pCreated);
	if (!bStored || pStored == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to persist the device certificate (%1)")
				.arg(GetLastError());
		return false;
	}
	KDeviceCertificate certificate;
	if (!CertificateData(pStored, m_identity.strDeviceId, &certificate, pErrorMessage))
	{
		CertFreeCertificateContext(pStored);
		return false;
	}
	m_pCertificateContext = const_cast<PCERT_CONTEXT>(pStored);
	m_certificate = certificate;
	return true;
}

void KWindowsDeviceIdentityProvider::closeKey()
{
	if (m_hKey != 0)
	{
		NCryptFreeObject(m_hKey);
		m_hKey = 0;
	}
}

void KWindowsDeviceIdentityProvider::closeCertificate()
{
	if (m_pCertificateContext != nullptr)
	{
		CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(m_pCertificateContext));
		m_pCertificateContext = nullptr;
	}
	m_certificate = KDeviceCertificate();
}
