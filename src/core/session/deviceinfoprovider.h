#ifndef _WINREMOTECONTROL_DEVICEINFOPROVIDER_H_
#define _WINREMOTECONTROL_DEVICEINFOPROVIDER_H_

#include "core/protocol/sessionmessage.h"

class IKDeviceInfoProvider
{
public:
	virtual ~IKDeviceInfoProvider() = default;

	virtual QString deviceName() = 0;
	virtual KRemoteDeviceInfo deviceInfo() = 0;
};

#endif // _WINREMOTECONTROL_DEVICEINFOPROVIDER_H_
