#include "adapters/settings/qsettingsrecentdevicestore.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>

namespace
{
	constexpr char kRecentDevicesArray[] = "recentDevices";
}

KQSettingsRecentDeviceStore::KQSettingsRecentDeviceStore(const QString &strFilePath)
	: m_strFilePath(strFilePath)
{
}

KQSettingsRecentDeviceStore::~KQSettingsRecentDeviceStore()
{
}

QVector<KRecentDevice> KQSettingsRecentDeviceStore::loadDevices(QString *pError)
{
	QVector<KRecentDevice> devices;
	QSettings settings(m_strFilePath, QSettings::IniFormat);
	const int nCount = settings.beginReadArray(QString::fromLatin1(kRecentDevicesArray));
	for (int nIndex = 0; nIndex < nCount; ++nIndex)
	{
		settings.setArrayIndex(nIndex);
		KRecentDevice device;
		device.strDeviceId = settings.value(QStringLiteral("id")).toString();
		device.strAuthenticatedDeviceId = settings.value(
			QStringLiteral("authenticatedDeviceId")).toString();
		device.strDeviceName = settings.value(QStringLiteral("name")).toString().left(128);
		device.strHost = settings.value(QStringLiteral("host")).toString().trimmed();
		device.nSignalingPort = static_cast<quint16>(settings.value(QStringLiteral("port")).toUInt());
		device.nLastConnectedAtMs = settings.value(QStringLiteral("lastConnectedAtMs")).toLongLong();
		device.bIncoming = settings.value(QStringLiteral("incoming"), false).toBool();
		if (!device.strDeviceId.isEmpty()
			&& !device.strHost.isEmpty()
			&& (device.bIncoming || device.nSignalingPort != 0))
		{
			devices.append(device);
		}
	}
	settings.endArray();
	if (settings.status() != QSettings::NoError && pError != nullptr)
		*pError = QStringLiteral("读取最近设备失败");
	return devices;
}

bool KQSettingsRecentDeviceStore::saveDevices(
	const QVector<KRecentDevice> &devices,
	QString *pError)
{
	const QFileInfo fileInfo(m_strFilePath);
	if (!QDir().mkpath(fileInfo.absolutePath()))
	{
		if (pError != nullptr)
			*pError = QStringLiteral("无法创建最近设备配置目录");
		return false;
	}

	QSettings settings(m_strFilePath, QSettings::IniFormat);
	settings.remove(QString::fromLatin1(kRecentDevicesArray));
	settings.beginWriteArray(QString::fromLatin1(kRecentDevicesArray), devices.size());
	for (int nIndex = 0; nIndex < devices.size(); ++nIndex)
	{
		const KRecentDevice &device = devices.at(nIndex);
		settings.setArrayIndex(nIndex);
		settings.setValue(QStringLiteral("id"), device.strDeviceId);
		settings.setValue(QStringLiteral("authenticatedDeviceId"),
			device.strAuthenticatedDeviceId);
		settings.setValue(QStringLiteral("name"), device.strDeviceName);
		settings.setValue(QStringLiteral("host"), device.strHost);
		settings.setValue(QStringLiteral("port"), device.nSignalingPort);
		settings.setValue(QStringLiteral("lastConnectedAtMs"), device.nLastConnectedAtMs);
		settings.setValue(QStringLiteral("incoming"), device.bIncoming);
	}
	settings.endArray();
	settings.sync();
	if (settings.status() == QSettings::NoError)
		return true;
	if (pError != nullptr)
		*pError = QStringLiteral("写入最近设备失败");
	return false;
}
