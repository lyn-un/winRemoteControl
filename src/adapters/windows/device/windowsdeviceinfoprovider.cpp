#include "adapters/windows/device/windowsdeviceinfoprovider.h"

#include <QtCore/QBuffer>
#include <QtCore/QSysInfo>
#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <Windows.h>

#include <cmath>
#include <vector>

namespace
{
	constexpr int kWallpaperInitialMaxEdge = 640;
	constexpr int kWallpaperMinEdge = 240;
	constexpr int kWallpaperJpegQuality = 65;
	constexpr int kWallpaperMaxBase64Bytes = 96 * 1024;
	constexpr int kWallpaperPathBufferLength = MAX_PATH;
	// Refresh rates of 0 or 1 mean "unknown" on virtual displays; anything
	// below this is not treated as a real monitor limit.
	constexpr double kMinimumPlausibleRefreshRateHz = 2.0;
	constexpr double kNominalRefreshRateToleranceHz = 1.0;
	constexpr int kNominalRefreshRates[] = { 24, 30, 60, 90, 120, 144 };

	static QString PrimaryDisplayName()
	{
		for (DWORD nDeviceIndex = 0;; ++nDeviceIndex)
		{
			DISPLAY_DEVICEW displayDevice = {};
			displayDevice.cb = sizeof(displayDevice);
			if (!::EnumDisplayDevicesW(nullptr, nDeviceIndex, &displayDevice, 0))
				return QString();
			if ((displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE)
				&& (displayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE))
			{
				return QString::fromWCharArray(displayDevice.DeviceName);
			}
		}
	}

	static double PrimaryDisplayRefreshRateHz()
	{
		const QString strPrimaryDisplayName = PrimaryDisplayName();
		if (strPrimaryDisplayName.isEmpty())
			return 0.0;

		for (int nAttempt = 0; nAttempt < 3; ++nAttempt)
		{
			UINT32 nPathCount = 0;
			UINT32 nModeCount = 0;
			const UINT32 nQueryFlags = QDC_ONLY_ACTIVE_PATHS;
			if (::GetDisplayConfigBufferSizes(nQueryFlags, &nPathCount, &nModeCount)
				!= ERROR_SUCCESS)
			{
				return 0.0;
			}

			std::vector<DISPLAYCONFIG_PATH_INFO> vecPaths(nPathCount);
			std::vector<DISPLAYCONFIG_MODE_INFO> vecModes(nModeCount);
			const LONG nQueryResult = ::QueryDisplayConfig(nQueryFlags,
				&nPathCount,
				vecPaths.data(),
				&nModeCount,
				vecModes.data(),
				nullptr);
			if (nQueryResult == ERROR_INSUFFICIENT_BUFFER)
				continue;
			if (nQueryResult != ERROR_SUCCESS)
				return 0.0;

			for (UINT32 nPathIndex = 0; nPathIndex < nPathCount; ++nPathIndex)
			{
				const DISPLAYCONFIG_PATH_INFO &path = vecPaths.at(nPathIndex);
				DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
				sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				sourceName.header.size = sizeof(sourceName);
				sourceName.header.adapterId = path.sourceInfo.adapterId;
				sourceName.header.id = path.sourceInfo.id;
				if (::DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS
					|| QString::fromWCharArray(sourceName.viewGdiDeviceName).compare(
						strPrimaryDisplayName, Qt::CaseInsensitive) != 0)
				{
					continue;
				}

				const DISPLAYCONFIG_RATIONAL &refreshRate = path.targetInfo.refreshRate;
				if (refreshRate.Denominator == 0)
					return 0.0;
				return static_cast<double>(refreshRate.Numerator)
					/ static_cast<double>(refreshRate.Denominator);
			}
			return 0.0;
		}
		return 0.0;
	}

}

KRemoteDeviceInfo KWindowsDeviceInfoProvider::deviceInfo()
{
	KRemoteDeviceInfo deviceInfo;
	deviceInfo.strComputerName = deviceName();
	deviceInfo.nScreenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	deviceInfo.nScreenHeight = ::GetSystemMetrics(SM_CYSCREEN);
	deviceInfo.strWallpaperData = readWallpaperBase64(&deviceInfo.strWallpaperMime);
	return deviceInfo;
}

int KWindowsDeviceInfoProvider::maximumFps()
{
	const double fRefreshRateHz = PrimaryDisplayRefreshRateHz();
	if (fRefreshRateHz >= kMinimumPlausibleRefreshRateHz)
		return normalizeRefreshRate(fRefreshRateHz);

	DEVMODEW deviceMode = {};
	deviceMode.dmSize = sizeof(DEVMODEW);
	if (!::EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &deviceMode, 0))
		return 0;
	if (!(deviceMode.dmFields & DM_DISPLAYFREQUENCY))
		return 0;
	const int nRefreshRate = static_cast<int>(deviceMode.dmDisplayFrequency);
	if (nRefreshRate < kMinimumPlausibleRefreshRateHz)
		return 0;
	return normalizeRefreshRate(static_cast<double>(nRefreshRate));
}

int KWindowsDeviceInfoProvider::normalizeRefreshRate(double fRefreshRateHz)
{
	if (fRefreshRateHz < kMinimumPlausibleRefreshRateHz)
		return 0;
	for (const int nNominalRefreshRate : kNominalRefreshRates)
	{
		if (std::abs(fRefreshRateHz - nNominalRefreshRate)
			<= kNominalRefreshRateToleranceHz)
		{
			return nNominalRefreshRate;
		}
	}
	return static_cast<int>(std::lround(fRefreshRateHz));
}

QString KWindowsDeviceInfoProvider::deviceName()
{
	return QSysInfo::machineHostName();
}

QString KWindowsDeviceInfoProvider::readWallpaperBase64(QString *pMimeType)
{
	wchar_t szWallpaperPath[kWallpaperPathBufferLength] = {};
	if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER,
			kWallpaperPathBufferLength,
			szWallpaperPath,
			0))
	{
		return QString();
	}

	const QString strWallpaperPath = QString::fromWCharArray(szWallpaperPath);
	if (strWallpaperPath.isEmpty())
		return QString();

	QImageReader imageReader(strWallpaperPath);
	QImage image = imageReader.read();
	if (image.isNull())
		return QString();

	const int nMaxEdge = qMax(image.width(), image.height());
	if (nMaxEdge > kWallpaperInitialMaxEdge)
	{
		image = image.scaled(kWallpaperInitialMaxEdge,
			kWallpaperInitialMaxEdge,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	QByteArray base64Data;
	for (;;)
	{
		QByteArray imageData;
		QBuffer buffer(&imageData);
		if (!buffer.open(QIODevice::WriteOnly))
			return QString();
		if (!image.save(&buffer, "JPG", kWallpaperJpegQuality))
			return QString();

		base64Data = imageData.toBase64();
		if (base64Data.size() <= kWallpaperMaxBase64Bytes)
			break;

		const int nCurrentMaxEdge = qMax(image.width(), image.height());
		if (nCurrentMaxEdge <= kWallpaperMinEdge)
			return QString();

		const int nNextMaxEdge = qMax(kWallpaperMinEdge, nCurrentMaxEdge * 3 / 4);
		image = image.scaled(nNextMaxEdge,
			nNextMaxEdge,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	if (pMimeType != nullptr)
		*pMimeType = QStringLiteral("image/jpeg");
	return QString::fromLatin1(base64Data);
}
