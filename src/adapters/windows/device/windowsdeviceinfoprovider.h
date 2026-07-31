#ifndef _WINREMOTECONTROL_WINDOWSDEVICEINFOPROVIDER_H_
#define _WINREMOTECONTROL_WINDOWSDEVICEINFOPROVIDER_H_

#include "core/session/deviceinfoprovider.h"

class KWindowsDeviceInfoProvider final : public IKDeviceInfoProvider
{
public:
	KRemoteDeviceInfo deviceInfo() override;

private:
	static QString readWallpaperBase64(QString *pMimeType);
};

#endif // _WINREMOTECONTROL_WINDOWSDEVICEINFOPROVIDER_H_
