#include "adapters/signaling/tcpsignalingtransport.h"

#include "common/latencytracelogger.h"
#include "core/protocol/protocolconstraints.h"

#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace
{
	constexpr int kConnectTimeoutMs = 3000;
	constexpr int kInitialReadTimeoutMs = 5000;
}

KTcpSignalingTransport::KTcpSignalingTransport(QObject *pParent)
	: KSignalingTransport(pParent)
	, m_pConnectTimeoutTimer(new QTimer(this))
	, m_pReadTimeoutTimer(new QTimer(this))
{
	m_pConnectTimeoutTimer->setSingleShot(true);
	m_pReadTimeoutTimer->setSingleShot(true);
	connect(m_pConnectTimeoutTimer, &QTimer::timeout,
		this, &KTcpSignalingTransport::handleConnectTimeout);
	connect(m_pReadTimeoutTimer, &QTimer::timeout,
		this, &KTcpSignalingTransport::handleReadTimeout);
}

KTcpSignalingTransport::~KTcpSignalingTransport()
{
	stop();
}

bool KTcpSignalingTransport::startServer(quint16 nPort, QString *pErrorMessage)
{
	stop();

	m_pServer = new QTcpServer(this);
	m_pServer->setProxy(QNetworkProxy::NoProxy);
	connect(m_pServer, &QTcpServer::newConnection,
		this, &KTcpSignalingTransport::handleNewConnection);

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

void KTcpSignalingTransport::connectToHost(const QString &strHost, quint16 nPort)
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

void KTcpSignalingTransport::disconnectPeer()
{
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	m_pConnectTimeoutTimer->stop();
	m_pReadTimeoutTimer->stop();
	m_connectElapsedTimer.invalidate();
	closeSocket();
	m_readBuffer.clear();
	emit stateChanged(m_pServer != nullptr && m_pServer->isListening()
		? QStringLiteral("Listening")
		: QStringLiteral("Idle"));
}

void KTcpSignalingTransport::stop()
{
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	m_pConnectTimeoutTimer->stop();
	m_pReadTimeoutTimer->stop();
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

bool KTcpSignalingTransport::isConnected() const
{
	return m_pSocket != nullptr && m_pSocket->state() == QAbstractSocket::ConnectedState;
}

void KTcpSignalingTransport::sendMessage(const QString &strMessage)
{
	if (!isConnected())
		return;

	QByteArray data = strMessage.toUtf8();
	if (data.size() > KProtocolConstraints::kMaximumSignalingMessageBytes)
	{
		emit signalingError(QStringLiteral("Outgoing signaling message exceeds size limit"));
		return;
	}
	data.append('\n');
	m_pSocket->write(data);
	m_pSocket->flush();
}

void KTcpSignalingTransport::handleNewConnection()
{
	if (m_pServer == nullptr)
		return;

	QTcpSocket *pSocket = m_pServer->nextPendingConnection();
	if (pSocket == nullptr)
		return;

	pSocket->setProxy(QNetworkProxy::NoProxy);
	if (m_bPeerBusy || isConnected())
	{
		pSocket->write(QByteArrayLiteral("{\"type\":\"busy\"}\n"));
		pSocket->flush();
		pSocket->disconnectFromHost();
		pSocket->deleteLater();
		return;
	}

	closeSocket();
	setSocket(pSocket);
	m_bPeerBusy = true;
	m_pReadTimeoutTimer->start(kInitialReadTimeoutMs);
	emit stateChanged(QStringLiteral("Connected"));
	emit incomingConnectionEstablished(pSocket->peerAddress().toString(), pSocket->peerPort());
}

void KTcpSignalingTransport::handleReadyRead()
{
	if (m_pSocket == nullptr)
		return;

	while (m_pSocket != nullptr && m_pSocket->bytesAvailable() > 0)
	{
		const qsizetype nRemainingBytes = KProtocolConstraints::kMaximumSignalingMessageBytes
			+ 1 - m_readBuffer.size();
		if (nRemainingBytes <= 0)
		{
			rejectPeerData(QStringLiteral("Signaling receive buffer exceeds size limit"));
			return;
		}
		m_readBuffer.append(m_pSocket->read(nRemainingBytes));
		for (;;)
		{
			const qsizetype nLineEnd = m_readBuffer.indexOf('\n');
			if (nLineEnd < 0)
				break;
			if (nLineEnd > KProtocolConstraints::kMaximumSignalingMessageBytes)
			{
				rejectPeerData(QStringLiteral("Signaling message exceeds size limit"));
				return;
			}

			const QByteArray line = m_readBuffer.left(nLineEnd).trimmed();
			m_readBuffer.remove(0, nLineEnd + 1);
			if (line.isEmpty())
				continue;

			m_pReadTimeoutTimer->stop();
			emit messageReceived(QString::fromUtf8(line));
		}

		if (m_readBuffer.size() > KProtocolConstraints::kMaximumSignalingMessageBytes)
		{
			rejectPeerData(QStringLiteral("Signaling receive buffer exceeds size limit"));
			return;
		}
	}
}

void KTcpSignalingTransport::handleConnected()
{
	if (m_bOutgoingConnectionPending)
	{
		m_bOutgoingConnectionPending = false;
		m_pConnectTimeoutTimer->stop();
		m_pReadTimeoutTimer->start(kInitialReadTimeoutMs);
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

void KTcpSignalingTransport::handleDisconnected()
{
	m_pReadTimeoutTimer->stop();
	emit stateChanged(QStringLiteral("Disconnected"));
	emit connectionLost();
}

void KTcpSignalingTransport::handleSocketError()
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

void KTcpSignalingTransport::handleConnectTimeout()
{
	if (!m_bOutgoingConnectionPending)
		return;

	failOutgoingConnection(QStringLiteral("timeout"),
		QStringLiteral("Signaling connection timed out after %1 ms").arg(kConnectTimeoutMs));
}

void KTcpSignalingTransport::handleReadTimeout()
{
	if (!isConnected())
		return;
	rejectPeerData(QStringLiteral("Signaling handshake timed out"));
}

void KTcpSignalingTransport::setSocket(QTcpSocket *pSocket)
{
	m_readBuffer.clear();
	m_pSocket = pSocket;
	connect(m_pSocket, &QTcpSocket::readyRead,
		this, &KTcpSignalingTransport::handleReadyRead);
	connect(m_pSocket, &QTcpSocket::connected,
		this, &KTcpSignalingTransport::handleConnected);
	connect(m_pSocket, &QTcpSocket::disconnected,
		this, &KTcpSignalingTransport::handleDisconnected);
	connect(m_pSocket, &QTcpSocket::errorOccurred,
		this, &KTcpSignalingTransport::handleSocketError);
}

void KTcpSignalingTransport::closeSocket()
{
	m_pReadTimeoutTimer->stop();
	if (m_pSocket == nullptr)
		return;

	m_pSocket->disconnect(this);
	m_pSocket->disconnectFromHost();
	m_pSocket->deleteLater();
	m_pSocket = nullptr;
}

void KTcpSignalingTransport::rejectPeerData(const QString &strMessage)
{
	m_pReadTimeoutTimer->stop();
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	m_readBuffer.clear();
	closeSocket();
	emit signalingError(strMessage);
	emit stateChanged(m_pServer != nullptr && m_pServer->isListening()
		? QStringLiteral("Listening")
		: QStringLiteral("ConnectionFailed"));
	emit connectionLost();
}

void KTcpSignalingTransport::failOutgoingConnection(const QString &strReason, const QString &strMessage)
{
	if (!m_bOutgoingConnectionPending)
		return;

	m_bOutgoingConnectionPending = false;
	m_pConnectTimeoutTimer->stop();
	m_pReadTimeoutTimer->stop();
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
