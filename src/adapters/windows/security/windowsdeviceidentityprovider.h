#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_WINDOWSDEVICEIDENTITYPROVIDER_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_WINDOWSDEVICEIDENTITYPROVIDER_H_

#include "core/security/deviceidentityprovider.h"

#include <QtCore/QString>
#include <windows.h>
#include <ncrypt.h>

class KWindowsDeviceIdentityProvider final : public KDeviceIdentityProvider
{
public:
	explicit KWindowsDeviceIdentityProvider(const QString &strSecurityDirectory);
	~KWindowsDeviceIdentityProvider() override;

	KWindowsDeviceIdentityProvider(const KWindowsDeviceIdentityProvider &) = delete;
	KWindowsDeviceIdentityProvider &operator=(const KWindowsDeviceIdentityProvider &) = delete;

	bool initialize(QString *pErrorMessage) override;
	KDeviceIdentity identity() const override;
	bool sign(const QByteArray &data,
		QByteArray *pSignature,
		QString *pErrorMessage) const override;
	bool verify(const QByteArray &publicKey,
		const QByteArray &data,
		const QByteArray &signature,
		QString *pErrorMessage) const override;
	QByteArray randomBytes(int nByteCount,
		QString *pErrorMessage) const override;

	bool deletePersistedKey(QString *pErrorMessage);

private:
	bool loadDescriptor(QString *pErrorMessage);
	bool createIdentity(QString *pErrorMessage);
	bool openPersistedKey(const QString &strDeviceId,
		bool *pMissing,
		QString *pErrorMessage);
	bool exportPublicKey(QByteArray *pPublicKey, QString *pErrorMessage) const;
	bool saveDescriptor(QString *pErrorMessage) const;
	void closeKey();

	QString m_strSecurityDirectory;
	KDeviceIdentity m_identity;
	NCRYPT_PROV_HANDLE m_hProvider = 0;
	NCRYPT_KEY_HANDLE m_hKey = 0;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_WINDOWSDEVICEIDENTITYPROVIDER_H_
