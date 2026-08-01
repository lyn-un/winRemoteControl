#ifndef _WINREMOTECONTROL_CORE_DEVICES_RECENTDEVICESTORE_H_
#define _WINREMOTECONTROL_CORE_DEVICES_RECENTDEVICESTORE_H_

#include "core/devices/recentdevice.h"

class KRecentDeviceStore
{
public:
	virtual ~KRecentDeviceStore() = default;

	virtual QVector<KRecentDevice> loadDevices(QString *pError) = 0;
	virtual bool saveDevices(const QVector<KRecentDevice> &devices, QString *pError) = 0;
};

#endif // _WINREMOTECONTROL_CORE_DEVICES_RECENTDEVICESTORE_H_
