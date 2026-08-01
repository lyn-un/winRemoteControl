#include "devices/recentdeviceservice.h"

#include "common/sessiontracelogger.h"
#include "core/devices/recentdevicestore.h"

#include <QtCore/QDateTime>
#include <QtCore/QUuid>

#include <algorithm>

KRecentDeviceService::KRecentDeviceService(
	std::unique_ptr<KRecentDeviceStore> pStore,
	QObject *pParent)
	: QObject(pParent)
	, m_pStore(std::move(pStore))
{
	Q_ASSERT(m_pStore != nullptr);
}

KRecentDeviceService::~KRecentDeviceService()
{
}

void KRecentDeviceService::initialize()
{
	QString strError;
	m_devices = m_pStore->loadDevices(&strError);
	sortAndTrimDevices();
	if (!strError.isEmpty())
		emit recentDeviceError(strError);
}

void KRecentDeviceService::requestDevices()
{
	emit devicesChanged(m_devices);
}

void KRecentDeviceService::connectEndpoint(const QString &strHost, quint16 nPort)
{
	const QString strNormalizedHost = strHost.trimmed();
	if (strNormalizedHost.isEmpty() || nPort == 0)
	{
		emit recentDeviceError(QStringLiteral("连接地址或端口无效"));
		return;
	}

	m_strPendingHost = strNormalizedHost;
	m_nPendingPort = nPort;
	m_strPendingDeviceName.clear();
	m_bSessionChannelOpen = false;
	m_bPendingSaved = false;
	const QString strRequestedHost = m_strPendingHost;
	const quint16 nRequestedPort = m_nPendingPort;
	emit connectEndpointRequested(strRequestedHost, nRequestedPort);
}

void KRecentDeviceService::connectDevice(const QString &strDeviceId)
{
	const int nIndex = findDeviceById(strDeviceId);
	if (nIndex < 0)
	{
		emit recentDeviceError(QStringLiteral("最近设备记录不存在"));
		return;
	}

	const KRecentDevice device = m_devices.at(nIndex);
	writeTrace(QStringLiteral("recent_device_connect"),
		QStringLiteral("deviceId=%1 host=%2 port=%3")
			.arg(device.strDeviceId, device.strHost)
			.arg(device.nSignalingPort));
	connectEndpoint(device.strHost, device.nSignalingPort);
}

void KRecentDeviceService::removeDevice(const QString &strDeviceId)
{
	const int nIndex = findDeviceById(strDeviceId);
	if (nIndex < 0)
	{
		emit recentDeviceError(QStringLiteral("最近设备记录不存在"));
		return;
	}

	m_devices.removeAt(nIndex);
	persistDevices();
	writeTrace(QStringLiteral("recent_device_removed"),
		QStringLiteral("deviceId=%1").arg(strDeviceId));
	emit devicesChanged(m_devices);
}

void KRecentDeviceService::setSessionChannelOpen(bool bOpen)
{
	m_bSessionChannelOpen = bOpen;
	if (!bOpen)
	{
		clearPendingConnection();
		return;
	}

	savePendingDevice();
}

void KRecentDeviceService::setRemoteDeviceName(const QString &strDeviceName)
{
	m_strPendingDeviceName = strDeviceName.trimmed().left(128);
	if (m_bSessionChannelOpen)
		savePendingDevice();
}

void KRecentDeviceService::savePendingDevice()
{
	if (!m_bSessionChannelOpen
		|| m_bPendingSaved
		|| m_strPendingHost.isEmpty()
		|| m_strPendingDeviceName.isEmpty()
		|| m_nPendingPort == 0)
		return;

	const qint64 nConnectedAtMs = QDateTime::currentMSecsSinceEpoch();
	int nIndex = findDeviceByEndpoint(m_strPendingHost, m_nPendingPort);
	const bool bAdded = nIndex < 0;
	if (bAdded)
	{
		KRecentDevice device;
		device.strDeviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		device.strHost = m_strPendingHost;
		device.nSignalingPort = m_nPendingPort;
		m_devices.append(device);
		nIndex = m_devices.size() - 1;
	}

	KRecentDevice &device = m_devices[nIndex];
	device.strDeviceName = m_strPendingDeviceName;
	device.nLastConnectedAtMs = nConnectedAtMs;
	const QString strDeviceId = device.strDeviceId;
	const QString strHost = device.strHost;
	const quint16 nPort = device.nSignalingPort;

	sortAndTrimDevices();
	persistDevices();
	writeTrace(bAdded ? QStringLiteral("recent_device_saved")
		: QStringLiteral("recent_device_updated"),
		QStringLiteral("deviceId=%1 host=%2 port=%3")
			.arg(strDeviceId, strHost)
			.arg(nPort));
	emit devicesChanged(m_devices);
	m_bPendingSaved = true;
}

void KRecentDeviceService::persistDevices()
{
	QString strError;
	if (!m_pStore->saveDevices(m_devices, &strError))
		emit recentDeviceError(strError.isEmpty()
			? QStringLiteral("保存最近设备失败")
			: strError);
}

void KRecentDeviceService::clearPendingConnection()
{
	m_strPendingHost.clear();
	m_strPendingDeviceName.clear();
	m_nPendingPort = 0;
	m_bPendingSaved = false;
}

int KRecentDeviceService::findDeviceByEndpoint(const QString &strHost, quint16 nPort) const
{
	for (int nIndex = 0; nIndex < m_devices.size(); ++nIndex)
	{
		const KRecentDevice &device = m_devices.at(nIndex);
		if (device.nSignalingPort == nPort
			&& device.strHost.compare(strHost.trimmed(), Qt::CaseInsensitive) == 0)
		{
			return nIndex;
		}
	}
	return -1;
}

int KRecentDeviceService::findDeviceById(const QString &strDeviceId) const
{
	for (int nIndex = 0; nIndex < m_devices.size(); ++nIndex)
	{
		if (m_devices.at(nIndex).strDeviceId == strDeviceId)
			return nIndex;
	}
	return -1;
}

void KRecentDeviceService::sortAndTrimDevices()
{
	std::sort(m_devices.begin(), m_devices.end(),
		[](const KRecentDevice &left, const KRecentDevice &right)
		{
			return left.nLastConnectedAtMs > right.nLastConnectedAtMs;
		});
	while (m_devices.size() > kMaximumRecentDevices)
		m_devices.removeLast();
}

void KRecentDeviceService::writeTrace(const QString &strStage, const QString &strExtra) const
{
	KSessionTraceLogger::write(QStringLiteral("controller"),
		strStage,
		QStringLiteral("recent_device"),
		-1,
		strExtra);
}
