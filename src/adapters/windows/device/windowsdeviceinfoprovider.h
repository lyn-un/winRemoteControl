#ifndef _WINREMOTECONTROL_WINDOWSDEVICEINFOPROVIDER_H_
#define _WINREMOTECONTROL_WINDOWSDEVICEINFOPROVIDER_H_

#include "core/session/deviceinfoprovider.h"

class KWindowsDeviceInfoProviderTestAccess;

class KWindowsDeviceInfoProvider final : public IKDeviceInfoProvider
{
public:
	QString deviceName() override;
	KRemoteDeviceInfo deviceInfo() override;
	int maximumFps() override;

private:
	static int normalizeRefreshRate(double fRefreshRateHz);
	static QString readWallpaperBase64(QString *pMimeType);

	friend class KWindowsDeviceInfoProviderTestAccess;
};

#endif // _WINREMOTECONTROL_WINDOWSDEVICEINFOPROVIDER_H_
