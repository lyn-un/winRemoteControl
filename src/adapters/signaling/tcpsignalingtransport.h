#ifndef _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_
#define _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_

#include "adapters/signaling/schanneltlsengine.h"
#include "core/transport/signalingtransport.h"

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QString>

class KDeviceIdentityProvider;
class QTcpServer;
class QTcpSocket;
class QTimer;

class KTcpSignalingTransport : public KSignalingTransport
{
	Q_OBJECT

public:
	explicit KTcpSignalingTransport(QObject *pParent = nullptr);
	~KTcpSignalingTransport() override;

	KTcpSignalingTransport(const KTcpSignalingTransport &) = delete;
	KTcpSignalingTransport &operator=(const KTcpSignalingTransport &) = delete;

	bool startServer(quint16 nPort, QString *pErrorMessage) override;
	void connectToHost(const QString &strHost, quint16 nPort) override;
	void disconnectPeer() override;
	void stop() override;
	void setServerBusyMessage(const QString &strMessage) override;
	bool setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider,
		QString *pErrorMessage) override;
	bool exportKeyingMaterial(const QByteArray &label,
		const QByteArray &context,
		int nLength,
		QByteArray *pKeyingMaterial,
		QString *pErrorMessage) override;
	bool isConnected() const;

public slots:
	void sendMessage(const QString &strMessage) override;

private slots:
	void handleNewConnection();
	void handleReadyRead();
	void handleConnected();
	void handleDisconnected();
	void handleSocketError();
	void handleConnectTimeout();
	void handleReadTimeout();

private:
	enum ConnectionStage
	{
		IdleStage,
		AwaitingClientPrefaceStage,
		AwaitingServerPrefaceStage,
		TlsHandshakeStage,
		SecureStage
	};

	void setSocket(QTcpSocket *pSocket);
	void closeSocket();
	void writeRaw(const QByteArray &data);
	bool beginTls(bool bServer, QString *pErrorMessage);
	bool processPreface(QString *pErrorMessage);
	bool processTls(QString *pErrorMessage);
	bool processPlaintext(QString *pErrorMessage);
	void completeSecureConnection();
	void failOutgoingConnection(const QString &strReason, const QString &strMessage);
	void rejectPeerData(const QString &strMessage, bool bTlsFailure = false);

	QTcpServer *m_pServer = nullptr;
	QTcpSocket *m_pSocket = nullptr;
	QTimer *m_pConnectTimeoutTimer = nullptr;
	QTimer *m_pReadTimeoutTimer = nullptr;
	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	KSchannelTlsEngine m_tlsEngine;
	QByteArray m_encryptedBuffer;
	QByteArray m_plaintextBuffer;
	QByteArray m_serverBusyMessage;
	QElapsedTimer m_connectElapsedTimer;
	ConnectionStage m_stage = IdleStage;
	bool m_bOutgoing = false;
	bool m_bOutgoingConnectionPending = false;
	bool m_bPeerBusy = false;
};

#endif // _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_
