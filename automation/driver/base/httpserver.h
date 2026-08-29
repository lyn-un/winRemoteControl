#ifndef _WINREMOTECONTROL_DRIVER_HTTPSERVER_H_
#define _WINREMOTECONTROL_DRIVER_HTTPSERVER_H_

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QTcpServer>

class QTcpSocket;
class QTimer;

class KHttpServer : public QObject
{
	Q_OBJECT

public:
	explicit KHttpServer(QObject *pParent = nullptr);
	~KHttpServer() override;

	KHttpServer(const KHttpServer &) = delete;
	KHttpServer &operator=(const KHttpServer &) = delete;

	bool start(const QString &strDiscoveryDirectory,
		qint64 nPid,
		const QString &strBuildId,
		QString *pErrorMessage);
	void stop();
	quint16 port() const;
	QString token() const;
	void sendJsonResponse(quint64 nRequestId, int nStatusCode, const QJsonObject &response);

signals:
	void requestReceived(quint64 nRequestId,
		const QByteArray &method,
		const QByteArray &path,
		const QByteArray &body);

private:
	struct KConnection
	{
		QByteArray buffer;
		bool bHeadersParsed = false;
		QTimer *pReadTimer = nullptr;
	};

	void handleNewConnection();
	void handleReadyRead(QTcpSocket *pSocket);
	bool tryParseRequest(QTcpSocket *pSocket, KConnection *pConnection);
	void reject(QTcpSocket *pSocket, int nStatusCode, const QString &strError);
	bool writeDiscoveryFile(const QString &strDiscoveryDirectory,
		qint64 nPid,
		const QString &strBuildId,
		QString *pErrorMessage);
	void removeDiscoveryFile();

	QTcpServer m_server;
	QHash<QTcpSocket *, KConnection> m_connections;
	QHash<quint64, QTcpSocket *> m_pendingRequests;
	QString m_strToken;
	QString m_strDiscoveryFilePath;
	quint64 m_nNextRequestId = 1;
};

#endif // _WINREMOTECONTROL_DRIVER_HTTPSERVER_H_
