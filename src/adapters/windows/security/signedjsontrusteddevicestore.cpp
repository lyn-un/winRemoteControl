#include "adapters/windows/security/signedjsontrusteddevicestore.h"

#include "adapters/windows/security/atomicjsonfile.h"
#include "core/security/deviceidentity.h"
#include "core/security/deviceidentityprovider.h"
#include "core/security/securitycanonicalwriter.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>
#include <algorithm>

namespace
{
	constexpr int kTrustStoreVersion = 3;
	constexpr int kMaximumTrustedDevices = 256;

	QString EncodeBase64Url(const QByteArray &value)
	{
		return QString::fromLatin1(value.toBase64(
			QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	}

	QByteArray DecodeBase64Url(const QString &strValue)
	{
		return QByteArray::fromBase64(strValue.toLatin1(),
			QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
	}

	QVector<KTrustedDevice> SortedDevices(const QVector<KTrustedDevice> &devices)
	{
		QVector<KTrustedDevice> sorted = devices;
		std::sort(sorted.begin(), sorted.end(),
			[](const KTrustedDevice &left, const KTrustedDevice &right)
			{
				return left.strDeviceId < right.strDeviceId;
			});
		return sorted;
	}

	QByteArray StoreData(const QVector<KTrustedDevice> &devices)
	{
		const QVector<KTrustedDevice> sorted = SortedDevices(devices);
		KSecurityCanonicalWriter writer;
		writer.appendString(QStringLiteral("wrc.trusted-devices"));
		writer.appendUInt32(kTrustStoreVersion);
		writer.appendUInt32(static_cast<quint32>(sorted.size()));
		for (const KTrustedDevice &device : sorted)
		{
			writer.appendString(device.strDeviceId);
			writer.appendBytes(device.spkiSha256);
			writer.appendBytes(device.certificateSha256);
			writer.appendString(device.strAlias);
			writer.appendString(device.strAdvertisedName);
			writer.appendUInt32(static_cast<quint32>(device.permissionLimit.toInt()));
			writer.appendUInt64(static_cast<quint64>(device.nPairedAtMs));
			writer.appendUInt64(static_cast<quint64>(device.nLastAuthenticatedAtMs));
			writer.appendBool(device.bRevoked);
			writer.appendUInt32(static_cast<quint32>(device.commitState));
			writer.appendString(device.strPairingTransactionId);
		}
		return writer.data();
	}

	bool IsValidDevice(const KTrustedDevice &device)
	{
		return !QUuid::fromString(device.strDeviceId).isNull()
			&& device.spkiSha256.size() == 32
			&& device.certificateSha256.size() == 32
			&& device.strFingerprint == QStringLiteral("SHA256:%1").arg(
				QString::fromLatin1(device.spkiSha256.toBase64(
					QByteArray::OmitTrailingEquals)))
			&& device.strAlias.size() <= 128
			&& device.strAdvertisedName.size() <= 128
			&& (static_cast<quint32>(device.permissionLimit.toInt()) & ~kAllPermissionScopeBits) == 0
			&& device.nPairedAtMs >= 0
			&& device.nLastAuthenticatedAtMs >= 0
			&& (device.commitState == PendingTrustedDeviceCommitState
				|| device.commitState == MutualTrustedDeviceCommitState)
			&& !QUuid(device.strPairingTransactionId).isNull();
	}
}

KSignedJsonTrustedDeviceStore::KSignedJsonTrustedDeviceStore(
	const QString &strFilePath)
	: m_strFilePath(strFilePath)
{
}

void KSignedJsonTrustedDeviceStore::setIdentityProvider(
	KDeviceIdentityProvider *pIdentityProvider)
{
	m_pIdentityProvider = pIdentityProvider;
}

QVector<KTrustedDevice> KSignedJsonTrustedDeviceStore::loadDevices(
	QString *pErrorMessage)
{
	QVector<KTrustedDevice> devices;
	if (!QFile::exists(m_strFilePath))
		return devices;
	if (m_pIdentityProvider == nullptr || !m_pIdentityProvider->identity().isValid())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Device identity is unavailable");
		return devices;
	}
	QFile file(m_strFilePath);
	if (!file.open(QIODevice::ReadOnly))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to open trusted device store");
		return devices;
	}
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
	file.close();
	const QJsonObject root = document.object();
	const QJsonArray array = root.value(QStringLiteral("devices")).toArray();
	const int nVersion = root.value(QStringLiteral("version")).toInt();
	if (parseError.error == QJsonParseError::NoError
		&& (nVersion == 1 || nVersion == 2))
	{
		const QString strBackupPath = QStringLiteral("%1.v%2.bak")
			.arg(m_strFilePath).arg(nVersion);
		if (!QFile::exists(strBackupPath) && !QFile::copy(m_strFilePath, strBackupPath))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Unable to back up legacy trust store");
			return {};
		}
		if (!saveDevices({}, pErrorMessage))
			return {};
		m_strMigrationNotice = QStringLiteral(
			"安全配对事务已升级，需要重新配对设备。旧信任库已备份。");
		return {};
	}
	if (parseError.error != QJsonParseError::NoError
		|| nVersion != kTrustStoreVersion
		|| array.size() > kMaximumTrustedDevices)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("trust_store_tampered");
		return QVector<KTrustedDevice>();
	}
	for (const QJsonValue &value : array)
	{
		const QJsonObject object = value.toObject();
		KTrustedDevice device;
		device.strDeviceId = object.value(QStringLiteral("deviceId")).toString();
		device.spkiSha256 = DecodeBase64Url(
			object.value(QStringLiteral("spkiSha256")).toString());
		device.certificateSha256 = DecodeBase64Url(
			object.value(QStringLiteral("certificateSha256")).toString());
		device.strFingerprint = QStringLiteral("SHA256:%1").arg(QString::fromLatin1(
			device.spkiSha256.toBase64(QByteArray::OmitTrailingEquals)));
		device.strAlias = object.value(QStringLiteral("alias")).toString();
		device.strAdvertisedName = object.value(QStringLiteral("advertisedName")).toString();
		device.permissionLimit = KPermissionScopes::fromInt(
			object.value(QStringLiteral("permissionBits")).toInt());
		device.nPairedAtMs = object.value(QStringLiteral("pairedAtMs")).toVariant().toLongLong();
		device.nLastAuthenticatedAtMs =
			object.value(QStringLiteral("lastAuthenticatedAtMs")).toVariant().toLongLong();
		device.bRevoked = object.value(QStringLiteral("revoked")).toBool();
		device.commitState = static_cast<KTrustedDeviceCommitState>(
			object.value(QStringLiteral("commitState")).toInt(-1));
		device.strPairingTransactionId = object.value(
			QStringLiteral("pairingTransactionId")).toString();
		if (!IsValidDevice(device))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("trust_store_tampered");
			return QVector<KTrustedDevice>();
		}
		devices.append(device);
	}
	const QByteArray signature = DecodeBase64Url(root.value(QStringLiteral("signature")).toString());
	QString strVerificationError;
	if (!m_pIdentityProvider->verify(m_pIdentityProvider->identity().publicKey,
		StoreData(devices), signature, &strVerificationError))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("trust_store_tampered: %1").arg(strVerificationError);
		return QVector<KTrustedDevice>();
	}
	return devices;
}

QString KSignedJsonTrustedDeviceStore::takeMigrationNotice()
{
	const QString strNotice = m_strMigrationNotice;
	m_strMigrationNotice.clear();
	return strNotice;
}

bool KSignedJsonTrustedDeviceStore::saveDevices(
	const QVector<KTrustedDevice> &devices,
	QString *pErrorMessage)
{
	if (m_pIdentityProvider == nullptr || !m_pIdentityProvider->identity().isValid())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Device identity is unavailable");
		return false;
	}
	if (devices.size() > kMaximumTrustedDevices)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Too many trusted devices");
		return false;
	}
	for (const KTrustedDevice &device : devices)
	{
		if (!IsValidDevice(device))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Invalid trusted device record");
			return false;
		}
	}
	QByteArray signature;
	if (!m_pIdentityProvider->sign(StoreData(devices), &signature, pErrorMessage))
		return false;
	QJsonArray array;
	for (const KTrustedDevice &device : SortedDevices(devices))
	{
		QJsonObject object;
		object.insert(QStringLiteral("deviceId"), device.strDeviceId);
		object.insert(QStringLiteral("spkiSha256"), EncodeBase64Url(device.spkiSha256));
		object.insert(QStringLiteral("certificateSha256"),
			EncodeBase64Url(device.certificateSha256));
		object.insert(QStringLiteral("alias"), device.strAlias);
		object.insert(QStringLiteral("advertisedName"), device.strAdvertisedName);
		object.insert(QStringLiteral("permissionBits"), device.permissionLimit.toInt());
		object.insert(QStringLiteral("pairedAtMs"), QString::number(device.nPairedAtMs));
		object.insert(QStringLiteral("lastAuthenticatedAtMs"),
			QString::number(device.nLastAuthenticatedAtMs));
		object.insert(QStringLiteral("revoked"), device.bRevoked);
		object.insert(QStringLiteral("commitState"),
			static_cast<int>(device.commitState));
		object.insert(QStringLiteral("pairingTransactionId"),
			device.strPairingTransactionId);
		array.append(object);
	}
	QJsonObject root;
	root.insert(QStringLiteral("version"), kTrustStoreVersion);
	root.insert(QStringLiteral("devices"), array);
	root.insert(QStringLiteral("signature"), EncodeBase64Url(signature));
	const QFileInfo fileInfo(m_strFilePath);
	if (!QDir().mkpath(fileInfo.absolutePath()))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to create the trusted device directory");
		return false;
	}
	return KAtomicJsonFile::write(m_strFilePath,
		QJsonDocument(root).toJson(QJsonDocument::Indented), pErrorMessage);
}
