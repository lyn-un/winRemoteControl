#ifndef _WINREMOTECONTROL_DEVICEINFOPROVIDER_H_
#define _WINREMOTECONTROL_DEVICEINFOPROVIDER_H_

#include "core/protocol/sessionmessage.h"

class IKDeviceInfoProvider
{
public:
	virtual ~IKDeviceInfoProvider() = default;

	virtual QString deviceName() = 0;
	virtual KRemoteDeviceInfo deviceInfo() = 0;

	// Maximum sustainable capture/display frame rate of the local machine in FPS.
	// Returns 0 when the refresh rate is unknown; callers fall back to the
	// protocol implementation limit.
	virtual int maximumFps() = 0;
};

#endif // _WINREMOTECONTROL_DEVICEINFOPROVIDER_H_
