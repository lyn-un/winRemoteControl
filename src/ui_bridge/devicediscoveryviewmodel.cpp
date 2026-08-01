#include "ui_bridge/devicediscoveryviewmodel.h"

#include "core/discovery/devicediscoverycontroller.h"

KDeviceDiscoveryViewModel::KDeviceDiscoveryViewModel(
	KDeviceDiscoveryController *pController,
	QObject *pParent)
	: QObject(pParent)
	, m_pController(pController)
{
	Q_ASSERT(m_pController != nullptr);
	connect(m_pController, &KDeviceDiscoveryController::devicesChanged,
		this, &KDeviceDiscoveryViewModel::lanDevicesChanged);
	connect(m_pController, &KDeviceDiscoveryController::discoveryError,
		this, &KDeviceDiscoveryViewModel::lanDiscoveryError);
}

void KDeviceDiscoveryViewModel::setRole(const QString &strRole)
{
	KSessionRole role;
	if (KSessionStateMachine::roleFromString(strRole, &role))
		m_pController->setRole(role);
}

void KDeviceDiscoveryViewModel::refreshLanDevices()
{
	m_pController->refresh();
}

void KDeviceDiscoveryViewModel::connectLanDevice(const QString &strDeviceId)
{
	m_pController->connectDevice(strDeviceId);
}
