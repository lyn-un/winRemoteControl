#ifndef _WINREMOTECONTROL_CORE_DEVICES_RECENTDEVICE_H_
#define _WINREMOTECONTROL_CORE_DEVICES_RECENTDEVICE_H_

#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVector>

struct KRecentDevice
{
	QString strDeviceId;
	QString strDeviceName;
	QString strHost;
	quint16 nSignalingPort = 0;
	qint64 nLastConnectedAtMs = 0;
};

Q_DECLARE_METATYPE(KRecentDevice)
Q_DECLARE_METATYPE(QVector<KRecentDevice>)

#endif // _WINREMOTECONTROL_CORE_DEVICES_RECENTDEVICE_H_
