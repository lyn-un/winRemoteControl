#ifndef _WINREMOTECONTROL_WEBRTCSIGNALING_H_
#define _WINREMOTECONTROL_WEBRTCSIGNALING_H_

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

class KWebRtcSignaling : public QObject
{
	Q_OBJECT

public:
	explicit KWebRtcSignaling(QObject *pParent = nullptr);
	~KWebRtcSignaling() override;

	KWebRtcSignaling(const KWebRtcSignaling &) = delete;
	KWebRtcSignaling &operator=(const KWebRtcSignaling &) = delete;

	bool startServer(quint16 nPort, QString *pErrorMessage);
	void connectToHost(const QString &strHost, quint16 nPort);
	void disconnectPeer();
	void stop();
	bool isConnected() const;

public slots:
	void sendJsonMessage(const QString &strMessage);

signals:
	void messageReceived(const QString &strMessage);
	void stateChanged(const QString &strState);
	void signalingError(const QString &strMessage);
	void outgoingConnectionEstablished();
	void outgoingConnectionFailed(const QString &strMessage);
	void incomingConnectionEstablished();
	void connectionLost();

private slots:
	void handleNewConnection();
	void handleReadyRead();
	void handleConnected();
	void handleDisconnected();
	void handleSocketError();
	void handleConnectTimeout();

private:
	void setSocket(QTcpSocket *pSocket);
	void closeSocket();
	void failOutgoingConnection(const QString &strReason, const QString &strMessage);

	QTcpServer *m_pServer = nullptr;
	QTcpSocket *m_pSocket = nullptr;
	QTimer *m_pConnectTimeoutTimer = nullptr;
	QByteArray m_readBuffer;
	QElapsedTimer m_connectElapsedTimer;
	bool m_bOutgoingConnectionPending = false;
	bool m_bPeerBusy = false;
};

#endif // _WINREMOTECONTROL_WEBRTCSIGNALING_H_
