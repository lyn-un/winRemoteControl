#ifndef _WINREMOTECONTROL_DISCOVERY_LANDISCOVERYSERVICE_H_
#define _WINREMOTECONTROL_DISCOVERY_LANDISCOVERYSERVICE_H_

#include "core/discovery/devicediscoverycontroller.h"
#include "core/discovery/landiscoverytransport.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

#include <memory>

struct KLanDiscoveryMessage;

class KLanDiscoveryService final : public KDeviceDiscoveryController
{
	Q_OBJECT

public:
	explicit KLanDiscoveryService(std::unique_ptr<KLanDiscoveryTransport> pTransport,
		const QString &strInstanceId,
		const QString &strDeviceName,
		QObject *pParent = nullptr);
	~KLanDiscoveryService() override;

public slots:
	void setRole(KSessionRole role) override;
	void setListeningAvailability(bool bAvailable, quint16 nPort) override;
	void refresh() override;
	void connectDevice(const QString &strDeviceId) override;
	void stop() override;

private slots:
	void handleDatagram(const QByteArray &data,
		const QString &strSenderHost,
		quint16 nSenderPort);
	void removeExpiredDevices();
	void handleTransportError(const QString &strError);

private:
	static constexpr quint16 kDiscoveryPort = 39001;
	static constexpr int kProbeIntervalMs = 2000;
	static constexpr int kDeviceExpiryMs = 6000;

	bool startTransport(quint16 nLocalPort);
	void startControllerDiscovery();
	void startControlledResponder();
	void sendProbe();
	void handleProbe(const KLanDiscoveryMessage &message,
		const QString &strSenderHost,
		quint16 nSenderPort);
	void handleAnnouncement(const KLanDiscoveryMessage &message,
		const QString &strSenderHost);
	void clearDevices();
	void publishDevices();
	void writeTrace(const QString &strStage, const QString &strExtra = QString()) const;

	std::unique_ptr<KLanDiscoveryTransport> m_pTransport;
	QString m_strInstanceId;
	QString m_strDeviceName;
	KSessionRole m_role = ControllerSessionRole;
	bool m_bRunning = false;
	bool m_bListeningAvailable = false;
	quint16 m_nSignalingPort = 0;
	QElapsedTimer m_clock;
	QTimer m_probeTimer;
	QTimer m_expiryTimer;
	QStringList m_recentRequestIds;
	QHash<QString, KDiscoveredDevice> m_devices;
};

#endif // _WINREMOTECONTROL_DISCOVERY_LANDISCOVERYSERVICE_H_
