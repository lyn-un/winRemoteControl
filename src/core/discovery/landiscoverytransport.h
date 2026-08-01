#ifndef _WINREMOTECONTROL_CORE_DISCOVERY_LANDISCOVERYTRANSPORT_H_
#define _WINREMOTECONTROL_CORE_DISCOVERY_LANDISCOVERYTRANSPORT_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

class KLanDiscoveryTransport : public QObject
{
	Q_OBJECT

public:
	explicit KLanDiscoveryTransport(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KLanDiscoveryTransport() override = default;

	KLanDiscoveryTransport(const KLanDiscoveryTransport &) = delete;
	KLanDiscoveryTransport &operator=(const KLanDiscoveryTransport &) = delete;

	virtual bool start(quint16 nLocalPort, QString *pErrorMessage) = 0;
	virtual void sendBroadcast(const QByteArray &data, quint16 nPort) = 0;
	virtual void sendUnicast(const QByteArray &data,
		const QString &strHost,
		quint16 nPort) = 0;
	virtual void stop() = 0;

signals:
	void datagramReceived(const QByteArray &data,
		const QString &strSenderHost,
		quint16 nSenderPort);
	void transportError(const QString &strError);
};

#endif // _WINREMOTECONTROL_CORE_DISCOVERY_LANDISCOVERYTRANSPORT_H_
