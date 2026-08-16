#ifndef _WINREMOTECONTROL_TESTS_UNIT_FAKESECURITY_H_
#define _WINREMOTECONTROL_TESTS_UNIT_FAKESECURITY_H_

#include "core/security/deviceidentityprovider.h"
#include "core/security/trusteddevicestore.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <memory>

class KFakeDeviceIdentityProvider final : public KDeviceIdentityProvider
{
public:
	KFakeDeviceIdentityProvider()
	{
		m_identity.strDeviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		m_identity.strAlgorithm = QStringLiteral("ecdsa-p256-sha256");
		m_identity.publicKey = QByteArray(65, '\0');
		m_identity.publicKey[0] = '\x04';
		const QByteArray idBytes = m_identity.strDeviceId.toUtf8();
		for (int nIndex = 1; nIndex < m_identity.publicKey.size(); ++nIndex)
			m_identity.publicKey[nIndex] = idBytes.at((nIndex - 1) % idBytes.size());
		m_identity.strFingerprint = DevicePublicKeyFingerprint(m_identity.publicKey);
		m_certificate.strDeviceId = m_identity.strDeviceId;
		m_certificate.certificateDer = QByteArrayLiteral("fake-certificate");
		m_certificate.spkiSha256 = QCryptographicHash::hash(
			m_identity.publicKey, QCryptographicHash::Sha256);
		m_certificate.certificateSha256 = QCryptographicHash::hash(
			m_certificate.certificateDer, QCryptographicHash::Sha256);
		m_certificate.validFromUtc = QDateTime::currentDateTimeUtc().addDays(-1);
		m_certificate.validToUtc = QDateTime::currentDateTimeUtc().addYears(1);
	}

	bool initialize(QString *) override { return true; }
	KDeviceIdentity identity() const override { return m_identity; }
	bool sign(const QByteArray &data, QByteArray *pSignature, QString *) const override
	{
		if (pSignature == nullptr)
			return false;
		const QByteArray digest = QCryptographicHash::hash(
			m_identity.publicKey + data, QCryptographicHash::Sha256);
		*pSignature = digest + digest;
		return true;
	}
	bool verify(const QByteArray &publicKey,
		const QByteArray &data,
		const QByteArray &signature,
		QString *) const override
	{
		const QByteArray digest = QCryptographicHash::hash(
			publicKey + data, QCryptographicHash::Sha256);
		return signature == digest + digest;
	}
	QByteArray randomBytes(int nByteCount, QString *) const override
	{
		QByteArray bytes(nByteCount, '\0');
		const QByteArray seed = QUuid::createUuid().toRfc4122();
		for (int nIndex = 0; nIndex < bytes.size(); ++nIndex)
			bytes[nIndex] = seed.at(nIndex % seed.size());
		return bytes;
	}
	KDeviceCertificate certificate() const override { return m_certificate; }
	void *duplicateNativeCertificate(QString *pErrorMessage) const override
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Native certificates are unavailable in fake security");
		return nullptr;
	}

private:
	KDeviceIdentity m_identity;
	KDeviceCertificate m_certificate;
};

class KFakeTrustedDeviceStore final : public KTrustedDeviceStore
{
public:
	void setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider) override
	{
		m_pIdentityProvider = pIdentityProvider;
	}
	QVector<KTrustedDevice> loadDevices(QString *) override { return m_devices; }
	bool saveDevices(const QVector<KTrustedDevice> &devices, QString *) override
	{
		m_devices = devices;
		return m_pIdentityProvider != nullptr;
	}

private:
	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	QVector<KTrustedDevice> m_devices;
};

inline std::unique_ptr<KDeviceIdentityProvider> MakeFakeIdentityProvider()
{
	return std::make_unique<KFakeDeviceIdentityProvider>();
}

inline std::unique_ptr<KTrustedDeviceStore> MakeFakeTrustedDeviceStore()
{
	return std::make_unique<KFakeTrustedDeviceStore>();
}

#endif // _WINREMOTECONTROL_TESTS_UNIT_FAKESECURITY_H_
