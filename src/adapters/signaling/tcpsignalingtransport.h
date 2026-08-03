#ifndef _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_
#define _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_

#include "core/transport/signalingtransport.h"

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QString>

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
	void setSocket(QTcpSocket *pSocket);
	void closeSocket();
	void failOutgoingConnection(const QString &strReason, const QString &strMessage);
	void rejectPeerData(const QString &strMessage);

	QTcpServer *m_pServer = nullptr;
	QTcpSocket *m_pSocket = nullptr;
	QTimer *m_pConnectTimeoutTimer = nullptr;
	QTimer *m_pReadTimeoutTimer = nullptr;
	QByteArray m_readBuffer;
	QElapsedTimer m_connectElapsedTimer;
	bool m_bOutgoingConnectionPending = false;
	bool m_bPeerBusy = false;
	int m_nConsecutiveInvalidMessages = 0;
};

#endif // _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_
