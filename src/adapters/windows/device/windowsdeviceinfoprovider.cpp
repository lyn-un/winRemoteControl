#include "adapters/windows/device/windowsdeviceinfoprovider.h"

#include <QtCore/QBuffer>
#include <QtCore/QSysInfo>
#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <Windows.h>

namespace
{
	constexpr int kWallpaperInitialMaxEdge = 640;
	constexpr int kWallpaperMinEdge = 240;
	constexpr int kWallpaperJpegQuality = 65;
	constexpr int kWallpaperMaxBase64Bytes = 96 * 1024;
	constexpr int kWallpaperPathBufferLength = MAX_PATH;
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
