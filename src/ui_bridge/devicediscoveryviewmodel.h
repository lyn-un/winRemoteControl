#ifndef _WINREMOTECONTROL_UI_BRIDGE_DEVICEDISCOVERYVIEWMODEL_H_
#define _WINREMOTECONTROL_UI_BRIDGE_DEVICEDISCOVERYVIEWMODEL_H_

#include "core/discovery/discovereddevice.h"

#include <QtCore/QObject>

class KDeviceDiscoveryController;

class KDeviceDiscoveryViewModel final : public QObject
{
	Q_OBJECT

public:
	explicit KDeviceDiscoveryViewModel(KDeviceDiscoveryController *pController,
		QObject *pParent = nullptr);

public slots:
	void setRole(const QString &strRole);
	void refreshLanDevices();
	void connectLanDevice(const QString &strDeviceId);

signals:
	void lanDevicesChanged(const QVector<KDiscoveredDevice> &devices);
	void lanDiscoveryError(const QString &strError);

private:
	KDeviceDiscoveryController *m_pController = nullptr;
};

#endif // _WINREMOTECONTROL_UI_BRIDGE_DEVICEDISCOVERYVIEWMODEL_H_
