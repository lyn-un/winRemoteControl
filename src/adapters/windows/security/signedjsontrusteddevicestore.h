#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_SIGNEDJSONTRUSTEDDEVICESTORE_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_SIGNEDJSONTRUSTEDDEVICESTORE_H_

#include "core/security/trusteddevicestore.h"

class KDeviceIdentityProvider;

class KSignedJsonTrustedDeviceStore final : public KTrustedDeviceStore
{
public:
	explicit KSignedJsonTrustedDeviceStore(const QString &strFilePath);

	void setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider) override;
	QVector<KTrustedDevice> loadDevices(QString *pErrorMessage) override;
	bool saveDevices(const QVector<KTrustedDevice> &devices,
		QString *pErrorMessage) override;
	QString takeMigrationNotice() override;

private:
	QString m_strFilePath;
	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	QString m_strMigrationNotice;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_SIGNEDJSONTRUSTEDDEVICESTORE_H_
