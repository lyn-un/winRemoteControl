#include "adapters/windows/device/windowsdeviceinfoprovider.h"

#include <iostream>

class KWindowsDeviceInfoProviderTestAccess
{
public:
	static int normalizeRefreshRate(double fRefreshRateHz)
	{
		return KWindowsDeviceInfoProvider::normalizeRefreshRate(fRefreshRateHz);
	}
};

namespace
{
	int g_nFailureCount = 0;

	void CheckNormalized(double fRefreshRateHz,
		int nExpectedFps,
		const char *pDescription)
	{
		if (KWindowsDeviceInfoProviderTestAccess::normalizeRefreshRate(fRefreshRateHz)
			== nExpectedFps)
		{
			return;
		}
		std::cerr << "FAILED: " << pDescription << '\n';
		++g_nFailureCount;
	}
}

int main()
{
	CheckNormalized(59.94, 60, "59.94 Hz normalizes to 60 FPS");
	CheckNormalized(60.01, 60, "60.01 Hz normalizes to 60 FPS");
	CheckNormalized(59.0, 60, "legacy 59 Hz mode normalizes to 60 FPS");
	CheckNormalized(89.0, 90, "89 Hz legacy mode normalizes to 90 FPS");
	CheckNormalized(89.91, 90, "fractional 90 Hz mode normalizes to 90 FPS");
	CheckNormalized(119.88, 120, "fractional 120 Hz mode normalizes to 120 FPS");
	CheckNormalized(143.86, 144, "fractional 144 Hz mode normalizes to 144 FPS");
	CheckNormalized(75.0, 75, "unknown standard refresh rate remains intact");
	CheckNormalized(200.0, 200, "non-standard high refresh rate remains intact");
	CheckNormalized(1.0, 0, "unknown refresh rate remains unavailable");
	return g_nFailureCount == 0 ? 0 : 1;
}
