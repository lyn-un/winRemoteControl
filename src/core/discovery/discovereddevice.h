#ifndef _WINREMOTECONTROL_CORE_DISCOVERY_DISCOVEREDDEVICE_H_
#define _WINREMOTECONTROL_CORE_DISCOVERY_DISCOVEREDDEVICE_H_

#include <QtCore/QString>
#include <QtCore/QMetaType>
#include <QtCore/QVector>

struct KDiscoveredDevice
{
	QString strDeviceId;
	QString strDeviceName;
	QString strHost;
	quint16 nSignalingPort = 0;
	qint64 nLastSeenMs = 0;
};

Q_DECLARE_METATYPE(KDiscoveredDevice)
Q_DECLARE_METATYPE(QVector<KDiscoveredDevice>)

#endif // _WINREMOTECONTROL_CORE_DISCOVERY_DISCOVEREDDEVICE_H_
