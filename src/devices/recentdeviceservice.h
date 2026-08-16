#ifndef _WINREMOTECONTROL_DEVICES_RECENTDEVICESERVICE_H_
#define _WINREMOTECONTROL_DEVICES_RECENTDEVICESERVICE_H_

#include "core/devices/recentdevice.h"

#include <QtCore/QObject>

#include <memory>

class KRecentDeviceStore;

class KRecentDeviceService final : public QObject
{
	Q_OBJECT

public:
	explicit KRecentDeviceService(std::unique_ptr<KRecentDeviceStore> pStore,
		QObject *pParent = nullptr);
	~KRecentDeviceService() override;

	KRecentDeviceService(const KRecentDeviceService &) = delete;
	KRecentDeviceService &operator=(const KRecentDeviceService &) = delete;

	void initialize();

public slots:
	void requestDevices();
	void connectEndpoint(const QString &strHost, quint16 nPort);
	void connectDevice(const QString &strDeviceId);
	void removeDevice(const QString &strDeviceId);
	void openTerminalDevice(const QString &strDeviceId);
	void prepareIncomingConnection(const QString &strDeviceName, const QString &strSourceAddress);
	void setSessionChannelOpen(bool bOpen);
	void setRemoteDeviceName(const QString &strDeviceName);
	void setAuthenticatedDeviceId(const QString &strDeviceId);

signals:
	void devicesChanged(const QVector<KRecentDevice> &devices);
	void recentDeviceError(const QString &strError);
	void connectEndpointRequested(const QString &strHost, quint16 nPort);
	void terminalEndpointRequested(const QString &strHost, quint16 nPort);

private:
	static constexpr int kMaximumRecentDevices = 8;

	void savePendingDevice();
	void persistDevices();
	void clearPendingConnection();
	int findPendingDevice() const;
	int findDeviceById(const QString &strDeviceId) const;
	void sortAndTrimDevices();
	void writeTrace(const QString &strStage, const QString &strExtra) const;

	std::unique_ptr<KRecentDeviceStore> m_pStore;
	QVector<KRecentDevice> m_devices;
	QString m_strPendingHost;
	QString m_strPendingDeviceName;
	QString m_strPendingAuthenticatedDeviceId;
	quint16 m_nPendingPort = 0;
	bool m_bPendingIncoming = false;
	bool m_bSessionChannelOpen = false;
	bool m_bPendingSaved = false;
};

#endif // _WINREMOTECONTROL_DEVICES_RECENTDEVICESERVICE_H_
