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
	constexpr int kTrustStoreVersion = 1;
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
			writer.appendBytes(device.publicKey);
			writer.appendString(device.strAlias);
			writer.appendString(device.strAdvertisedName);
			writer.appendUInt32(static_cast<quint32>(device.permissionLimit.toInt()));
			writer.appendUInt64(static_cast<quint64>(device.nPairedAtMs));
			writer.appendUInt64(static_cast<quint64>(device.nLastAuthenticatedAtMs));
			writer.appendBool(device.bRevoked);
		}
		return writer.data();
	}

	bool IsValidDevice(const KTrustedDevice &device)
	{
		return !QUuid::fromString(device.strDeviceId).isNull()
			&& device.publicKey.size() == 65
			&& device.publicKey.at(0) == '\x04'
			&& device.strFingerprint == DevicePublicKeyFingerprint(device.publicKey)
			&& device.strAlias.size() <= 128
			&& device.strAdvertisedName.size() <= 128
			&& (static_cast<quint32>(device.permissionLimit.toInt()) & ~kAllPermissionScopeBits) == 0
			&& device.nPairedAtMs >= 0
			&& device.nLastAuthenticatedAtMs >= 0;
	}
}

KSignedJsonTrustedDeviceStore::KSignedJsonTrustedDeviceStore(
	const QString &strFilePath,
	KDeviceIdentityProvider *pIdentityProvider)
	: m_strFilePath(strFilePath)
	, m_pIdentityProvider(pIdentityProvider)
{
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
	const QJsonObject root = document.object();
	const QJsonArray array = root.value(QStringLiteral("devices")).toArray();
	if (parseError.error != QJsonParseError::NoError
		|| root.value(QStringLiteral("version")).toInt() != kTrustStoreVersion
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
		device.publicKey = DecodeBase64Url(object.value(QStringLiteral("publicKey")).toString());
		device.strFingerprint = DevicePublicKeyFingerprint(device.publicKey);
		device.strAlias = object.value(QStringLiteral("alias")).toString();
		device.strAdvertisedName = object.value(QStringLiteral("advertisedName")).toString();
		device.permissionLimit = KPermissionScopes::fromInt(
			object.value(QStringLiteral("permissionBits")).toInt());
		device.nPairedAtMs = object.value(QStringLiteral("pairedAtMs")).toVariant().toLongLong();
		device.nLastAuthenticatedAtMs =
			object.value(QStringLiteral("lastAuthenticatedAtMs")).toVariant().toLongLong();
		device.bRevoked = object.value(QStringLiteral("revoked")).toBool();
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
		object.insert(QStringLiteral("publicKey"), EncodeBase64Url(device.publicKey));
		object.insert(QStringLiteral("alias"), device.strAlias);
		object.insert(QStringLiteral("advertisedName"), device.strAdvertisedName);
		object.insert(QStringLiteral("permissionBits"), device.permissionLimit.toInt());
		object.insert(QStringLiteral("pairedAtMs"), QString::number(device.nPairedAtMs));
		object.insert(QStringLiteral("lastAuthenticatedAtMs"),
			QString::number(device.nLastAuthenticatedAtMs));
		object.insert(QStringLiteral("revoked"), device.bRevoked);
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
