#ifndef _WINREMOTECONTROL_WEBRTCSIGNALING_H_
#define _WINREMOTECONTROL_WEBRTCSIGNALING_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

class QTcpServer;
class QTcpSocket;

class KWebRtcSignaling : public QObject
{
	Q_OBJECT

public:
	explicit KWebRtcSignaling(QObject *pParent = nullptr);
	~KWebRtcSignaling() override;

	KWebRtcSignaling(const KWebRtcSignaling &) = delete;
	KWebRtcSignaling &operator=(const KWebRtcSignaling &) = delete;

	bool startServer(quint16 nPort, QString *pErrorMessage);
	bool connectToHost(const QString &strHost, quint16 nPort, QString *pErrorMessage);
	void stop();
	bool isConnected() const;

public slots:
	void sendJsonMessage(const QString &strMessage);

signals:
	void messageReceived(const QString &strMessage);
	void stateChanged(const QString &strState);
	void signalingError(const QString &strMessage);

private slots:
	void handleNewConnection();
	void handleReadyRead();
	void handleConnected();
	void handleDisconnected();
	void handleSocketError();

private:
	void setSocket(QTcpSocket *pSocket);
	void closeSocket();

	QTcpServer *m_pServer = nullptr;
	QTcpSocket *m_pSocket = nullptr;
	QByteArray m_readBuffer;
};

#endif // _WINREMOTECONTROL_WEBRTCSIGNALING_H_
