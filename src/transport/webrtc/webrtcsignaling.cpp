#include "transport/webrtc/webrtcsignaling.h"

#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

KWebRtcSignaling::KWebRtcSignaling(QObject *pParent)
	: QObject(pParent)
{
}

KWebRtcSignaling::~KWebRtcSignaling()
{
	stop();
}

bool KWebRtcSignaling::startServer(quint16 nPort, QString *pErrorMessage)
{
	stop();

	m_pServer = new QTcpServer(this);
	m_pServer->setProxy(QNetworkProxy::NoProxy);
	connect(m_pServer, &QTcpServer::newConnection,
		this, &KWebRtcSignaling::handleNewConnection);

	if (!m_pServer->listen(QHostAddress::AnyIPv4, nPort))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = m_pServer->errorString();
		stop();
		return false;
	}

	emit stateChanged(QStringLiteral("Listening"));
	return true;
}

bool KWebRtcSignaling::connectToHost(const QString &strHost, quint16 nPort, QString *pErrorMessage)
{
	stop();

	QTcpSocket *pSocket = new QTcpSocket(this);
	pSocket->setProxy(QNetworkProxy::NoProxy);
	setSocket(pSocket);
	pSocket->connectToHost(strHost, nPort);
	if (!pSocket->waitForConnected(3000))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = pSocket->errorString();
		stop();
		return false;
	}

	return true;
}

void KWebRtcSignaling::stop()
{
	closeSocket();
	if (m_pServer != nullptr)
	{
		m_pServer->close();
		m_pServer->deleteLater();
		m_pServer = nullptr;
	}
	m_readBuffer.clear();
	emit stateChanged(QStringLiteral("Idle"));
}

bool KWebRtcSignaling::isConnected() const
{
	return m_pSocket != nullptr && m_pSocket->state() == QAbstractSocket::ConnectedState;
}

void KWebRtcSignaling::sendJsonMessage(const QString &strMessage)
{
	if (!isConnected())
		return;

	QByteArray data = strMessage.toUtf8();
	data.append('\n');
	m_pSocket->write(data);
	m_pSocket->flush();
}

void KWebRtcSignaling::handleNewConnection()
{
	if (m_pServer == nullptr)
		return;

	QTcpSocket *pSocket = m_pServer->nextPendingConnection();
	if (pSocket == nullptr)
		return;

	pSocket->setProxy(QNetworkProxy::NoProxy);
	closeSocket();
	setSocket(pSocket);
	emit stateChanged(QStringLiteral("Connected"));
}

void KWebRtcSignaling::handleReadyRead()
{
	if (m_pSocket == nullptr)
		return;

	m_readBuffer.append(m_pSocket->readAll());
	for (;;)
	{
		const int nLineEnd = m_readBuffer.indexOf('\n');
		if (nLineEnd < 0)
			break;

		const QByteArray line = m_readBuffer.left(nLineEnd).trimmed();
		m_readBuffer.remove(0, nLineEnd + 1);
		if (!line.isEmpty())
			emit messageReceived(QString::fromUtf8(line));
	}
}

void KWebRtcSignaling::handleConnected()
{
	emit stateChanged(QStringLiteral("Connected"));
}

void KWebRtcSignaling::handleDisconnected()
{
	emit stateChanged(QStringLiteral("Disconnected"));
}

void KWebRtcSignaling::handleSocketError()
{
	if (m_pSocket != nullptr)
		emit signalingError(m_pSocket->errorString());
}

void KWebRtcSignaling::setSocket(QTcpSocket *pSocket)
{
	m_pSocket = pSocket;
	connect(m_pSocket, &QTcpSocket::readyRead,
		this, &KWebRtcSignaling::handleReadyRead);
	connect(m_pSocket, &QTcpSocket::connected,
		this, &KWebRtcSignaling::handleConnected);
	connect(m_pSocket, &QTcpSocket::disconnected,
		this, &KWebRtcSignaling::handleDisconnected);
	connect(m_pSocket, &QTcpSocket::errorOccurred,
		this, &KWebRtcSignaling::handleSocketError);
}

void KWebRtcSignaling::closeSocket()
{
	if (m_pSocket == nullptr)
		return;

	m_pSocket->disconnect(this);
	m_pSocket->disconnectFromHost();
	m_pSocket->deleteLater();
	m_pSocket = nullptr;
}
