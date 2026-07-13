#include "transport/webrtc/webrtcsignaling.h"

#include "common/latencytracelogger.h"

#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace
{
	constexpr int kConnectTimeoutMs = 3000;
}

KWebRtcSignaling::KWebRtcSignaling(QObject *pParent)
	: QObject(pParent)
	, m_pConnectTimeoutTimer(new QTimer(this))
{
	m_pConnectTimeoutTimer->setSingleShot(true);
	connect(m_pConnectTimeoutTimer, &QTimer::timeout,
		this, &KWebRtcSignaling::handleConnectTimeout);
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

void KWebRtcSignaling::connectToHost(const QString &strHost, quint16 nPort)
{
	stop();

	QTcpSocket *pSocket = new QTcpSocket(this);
	pSocket->setProxy(QNetworkProxy::NoProxy);
	setSocket(pSocket);
	m_bOutgoingConnectionPending = true;
	m_connectElapsedTimer.start();
	m_pConnectTimeoutTimer->start(kConnectTimeoutMs);
	emit stateChanged(QStringLiteral("Connecting"));
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("signaling_connect_start"),
		QStringLiteral("host=%1 port=%2 timeoutMs=%3")
			.arg(strHost)
			.arg(nPort)
			.arg(kConnectTimeoutMs));
	pSocket->connectToHost(strHost, nPort);
}

void KWebRtcSignaling::stop()
{
	m_bOutgoingConnectionPending = false;
	m_pConnectTimeoutTimer->stop();
	m_connectElapsedTimer.invalidate();
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
	if (m_bOutgoingConnectionPending)
	{
		m_bOutgoingConnectionPending = false;
		m_pConnectTimeoutTimer->stop();
		const qint64 nCostMs = m_connectElapsedTimer.elapsed();
		m_connectElapsedTimer.invalidate();
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("signaling_connect_success"),
			QStringLiteral("costMs=%1").arg(nCostMs));
		emit stateChanged(QStringLiteral("Connected"));
		emit outgoingConnectionEstablished();
		return;
	}

	emit stateChanged(QStringLiteral("Connected"));
}

void KWebRtcSignaling::handleDisconnected()
{
	emit stateChanged(QStringLiteral("Disconnected"));
}

void KWebRtcSignaling::handleSocketError()
{
	if (m_pSocket == nullptr)
		return;

	const QString strError = m_pSocket->errorString();
	if (m_bOutgoingConnectionPending)
	{
		failOutgoingConnection(QStringLiteral("socket_error"), strError);
		return;
	}

	emit signalingError(strError);
}

void KWebRtcSignaling::handleConnectTimeout()
{
	if (!m_bOutgoingConnectionPending)
		return;

	failOutgoingConnection(QStringLiteral("timeout"),
		QStringLiteral("Signaling connection timed out after %1 ms").arg(kConnectTimeoutMs));
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

void KWebRtcSignaling::failOutgoingConnection(const QString &strReason, const QString &strMessage)
{
	if (!m_bOutgoingConnectionPending)
		return;

	m_bOutgoingConnectionPending = false;
	m_pConnectTimeoutTimer->stop();
	const qint64 nCostMs = m_connectElapsedTimer.elapsed();
	m_connectElapsedTimer.invalidate();
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("signaling_connect_failed"),
		QStringLiteral("costMs=%1 reason=%2 error=%3")
			.arg(nCostMs)
			.arg(strReason, strMessage));
	closeSocket();
	emit stateChanged(QStringLiteral("ConnectionFailed"));
	emit outgoingConnectionFailed(strMessage);
}
