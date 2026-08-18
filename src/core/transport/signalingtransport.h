#ifndef _WINREMOTECONTROL_SIGNALINGTRANSPORT_H_
#define _WINREMOTECONTROL_SIGNALINGTRANSPORT_H_

#include <QtCore/QObject>
#include <QtCore/QString>

#include "core/session/sessionerror.h"
#include "core/transport/keyingmaterialexporter.h"
#include "core/transport/tlspeeridentity.h"

class KDeviceIdentityProvider;
class KAdmissionController;

class KSignalingTransport : public QObject, public KKeyingMaterialExporter
{
	Q_OBJECT

public:
	explicit KSignalingTransport(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KSignalingTransport() override = default;

	KSignalingTransport(const KSignalingTransport &) = delete;
	KSignalingTransport &operator=(const KSignalingTransport &) = delete;

	virtual bool startServer(quint16 nPort, QString *pErrorMessage) = 0;
	virtual quint16 listeningPort() const { return 0; }
	virtual void setAdmissionController(KAdmissionController *) {}
	virtual void connectToHost(const QString &strHost, quint16 nPort) = 0;
	virtual void disconnectPeer() = 0;
	virtual void stop() = 0;
	virtual void setServerBusyMessage(const QString &strMessage) = 0;
	virtual bool setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider,
		QString *pErrorMessage)
	{
		Q_UNUSED(pIdentityProvider);
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Secure signaling is not supported");
		return false;
	}

public slots:
	virtual void sendMessage(const QString &strMessage) = 0;

signals:
	void messageReceived(const QString &strMessage);
	void stateChanged(const QString &strState);
	void signalingError(const QString &strMessage);
	void outgoingConnectionEstablished();
	void outgoingConnectionFailed(const QString &strMessage);
	void incomingConnectionEstablished(const QString &strSourceAddress, quint16 nSourcePort);
	void connectionLost();
	void secureChannelEstablished(const KTlsPeerIdentity &peer);
	void tlsHandshakeFailed(const KSessionError &error);
};

#endif // _WINREMOTECONTROL_SIGNALINGTRANSPORT_H_
