#ifndef _WINREMOTECONTROL_TESTS_UNIT_FAKESECURITY_H_
#define _WINREMOTECONTROL_TESTS_UNIT_FAKESECURITY_H_

#include "core/security/deviceidentityprovider.h"
#include "core/security/trusteddevicestore.h"
#include "core/transport/keyingmaterialexporter.h"

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

	bool initialize(QString *) override
	{
		++m_nInitializeCount;
		return true;
	}
	int initializeCount() const { return m_nInitializeCount; }
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
	int m_nInitializeCount = 0;
};

class KFakeTrustedDeviceStore final : public KTrustedDeviceStore
{
public:
	void setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider) override
	{
		m_pIdentityProvider = pIdentityProvider;
	}
	QVector<KTrustedDevice> loadDevices(QString *) override
	{
		++m_nLoadCount;
		return m_devices;
	}
	KTrustedDeviceStoreError lastLoadError() const override
	{
		return NoTrustedDeviceStoreError;
	}
	bool saveDevices(const QVector<KTrustedDevice> &devices, QString *pErrorMessage) override
	{
		++m_nSaveCount;
		if (m_pIdentityProvider == nullptr
			|| (m_nFailOnSaveCall > 0 && m_nSaveCount == m_nFailOnSaveCall))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Injected trusted-device save failure");
			return false;
		}
		m_devices = devices;
		return true;
	}

	void setDevices(const QVector<KTrustedDevice> &devices) { m_devices = devices; }
	const QVector<KTrustedDevice> &devices() const { return m_devices; }
	void failOnSaveCall(int nSaveCall) { m_nFailOnSaveCall = nSaveCall; }
	int saveCount() const { return m_nSaveCount; }
	int loadCount() const { return m_nLoadCount; }

private:
	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	QVector<KTrustedDevice> m_devices;
	int m_nSaveCount = 0;
	int m_nFailOnSaveCall = 0;
	int m_nLoadCount = 0;
};

class KFakeKeyingMaterialExporter final : public KKeyingMaterialExporter
{
public:
	bool exportKeyingMaterial(const QByteArray &label,
		const QByteArray &context,
		int nLength,
		QByteArray *pKeyingMaterial,
		QString *pErrorMessage) override
	{
		if (m_bFail || pKeyingMaterial == nullptr || nLength <= 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Fake TLS exporter failed");
			return false;
		}
		QByteArray material;
		quint32 nCounter = 0;
		while (material.size() < nLength)
		{
			QByteArray input = m_secret + label + context;
			input.append(static_cast<char>((nCounter >> 24) & 0xff));
			input.append(static_cast<char>((nCounter >> 16) & 0xff));
			input.append(static_cast<char>((nCounter >> 8) & 0xff));
			input.append(static_cast<char>(nCounter & 0xff));
			material.append(QCryptographicHash::hash(input, QCryptographicHash::Sha256));
			++nCounter;
		}
		*pKeyingMaterial = material.left(nLength);
		return true;
	}

	void setFailure(bool bFail) { m_bFail = bFail; }

private:
	QByteArray m_secret = QByteArrayLiteral("fake-tls-exporter-secret");
	bool m_bFail = false;
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
