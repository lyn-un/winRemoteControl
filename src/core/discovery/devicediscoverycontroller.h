#ifndef _WINREMOTECONTROL_CORE_DISCOVERY_DEVICEDISCOVERYCONTROLLER_H_
#define _WINREMOTECONTROL_CORE_DISCOVERY_DEVICEDISCOVERYCONTROLLER_H_

#include "core/discovery/discovereddevice.h"
#include "core/session/sessionstatemachine.h"

#include <QtCore/QObject>

class KDeviceDiscoveryController : public QObject
{
	Q_OBJECT

public:
	explicit KDeviceDiscoveryController(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KDeviceDiscoveryController() override = default;

public slots:
	virtual void setRole(KSessionRole role) = 0;
	virtual void setListeningAvailability(bool bAvailable, quint16 nPort) = 0;
	virtual void refresh() = 0;
	virtual void connectDevice(const QString &strDeviceId) = 0;
	virtual void stop() = 0;

signals:
	void devicesChanged(const QVector<KDiscoveredDevice> &devices);
	void discoveryError(const QString &strError);
	void connectEndpointRequested(const QString &strHost, quint16 nPort);
};

#endif // _WINREMOTECONTROL_CORE_DISCOVERY_DEVICEDISCOVERYCONTROLLER_H_
