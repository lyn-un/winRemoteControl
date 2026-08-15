#include "adapters/windows/security/windowsdeviceidentityprovider.h"

#include "adapters/windows/security/atomicjsonfile.h"
#include "core/security/securitycanonicalwriter.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>

namespace
{
	constexpr int kIdentityFormatVersion = 1;
	constexpr int kP256PublicKeyBytes = 65;
	constexpr int kP256SignatureBytes = 64;
	constexpr int kSha256Bytes = 32;

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
}

KWindowsDeviceIdentityProvider::KWindowsDeviceIdentityProvider(
	const QString &strSecurityDirectory)
	: m_strSecurityDirectory(strSecurityDirectory)
{
}

KWindowsDeviceIdentityProvider::~KWindowsDeviceIdentityProvider()
{
	closeKey();
	if (m_hProvider != 0)
		NCryptFreeObject(m_hProvider);
}

bool KWindowsDeviceIdentityProvider::initialize(QString *pErrorMessage)
{
	if (m_hKey != 0 && m_identity.isValid())
		return true;
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
	if (QFile::exists(DescriptorPath(m_strSecurityDirectory)))
		return loadDescriptor(pErrorMessage);
	return createIdentity(pErrorMessage);
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

bool KWindowsDeviceIdentityProvider::deletePersistedKey(QString *pErrorMessage)
{
	if (m_hKey == 0)
		return true;
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
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
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
	SECURITY_STATUS status = NCryptCreatePersistedKey(m_hProvider, &m_hKey,
		NCRYPT_ECDSA_P256_ALGORITHM, reinterpret_cast<LPCWSTR>(strKeyName.utf16()), 0, 0);
	if (status == ERROR_SUCCESS)
	{
		DWORD nExportPolicy = 0;
		status = NCryptSetProperty(m_hKey, NCRYPT_EXPORT_POLICY_PROPERTY,
			reinterpret_cast<PBYTE>(&nExportPolicy), sizeof(nExportPolicy), 0);
	}
	if (status == ERROR_SUCCESS)
		status = NCryptFinalizeKey(m_hKey, 0);
	if (status != ERROR_SUCCESS)
	{
		closeKey();
		if (pErrorMessage != nullptr)
			*pErrorMessage = StatusMessage(QStringLiteral("NCryptCreatePersistedKey"), status);
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

void KWindowsDeviceIdentityProvider::closeKey()
{
	if (m_hKey != 0)
	{
		NCryptFreeObject(m_hKey);
		m_hKey = 0;
	}
}
