#ifndef _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICESTORE_H_
#define _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICESTORE_H_

#include "core/security/trusteddevice.h"

class KTrustedDeviceStore
{
public:
	virtual ~KTrustedDeviceStore() = default;

	virtual QVector<KTrustedDevice> loadDevices(QString *pErrorMessage) = 0;
	virtual bool saveDevices(const QVector<KTrustedDevice> &devices,
		QString *pErrorMessage) = 0;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICESTORE_H_
