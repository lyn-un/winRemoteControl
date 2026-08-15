#ifndef _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICESTORE_H_
#define _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICESTORE_H_

#include "core/security/trusteddevice.h"

class KDeviceIdentityProvider;

class KTrustedDeviceStore
{
public:
	virtual ~KTrustedDeviceStore() = default;

	virtual void setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider) = 0;
	virtual QVector<KTrustedDevice> loadDevices(QString *pErrorMessage) = 0;
	virtual bool saveDevices(const QVector<KTrustedDevice> &devices,
		QString *pErrorMessage) = 0;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICESTORE_H_
