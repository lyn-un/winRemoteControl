#ifndef _WINREMOTECONTROL_ADAPTERS_DISCOVERY_UDPLANDISCOVERYTRANSPORT_H_
#define _WINREMOTECONTROL_ADAPTERS_DISCOVERY_UDPLANDISCOVERYTRANSPORT_H_

#include "core/discovery/landiscoverytransport.h"

class QUdpSocket;

class KUdpLanDiscoveryTransport final : public KLanDiscoveryTransport
{
	Q_OBJECT

public:
	explicit KUdpLanDiscoveryTransport(QObject *pParent = nullptr);
	~KUdpLanDiscoveryTransport() override = default;

	bool start(quint16 nLocalPort, QString *pErrorMessage) override;
	void sendBroadcast(const QByteArray &data, quint16 nPort) override;
	void sendUnicast(const QByteArray &data,
		const QString &strHost,
		quint16 nPort) override;
	void stop() override;

private slots:
	void readPendingDatagrams();
	void handleSocketError();

private:
	void sendDatagram(const QByteArray &data,
		const class QHostAddress &address,
		quint16 nPort);

	QUdpSocket *m_pSocket = nullptr;
};

#endif // _WINREMOTECONTROL_ADAPTERS_DISCOVERY_UDPLANDISCOVERYTRANSPORT_H_
