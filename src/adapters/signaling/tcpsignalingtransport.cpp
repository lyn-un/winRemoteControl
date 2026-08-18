#include "adapters/signaling/tcpsignalingtransport.h"

#include "core/security/admissioncontroller.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "core/protocol/protocolconstraints.h"
#include "core/security/deviceidentityprovider.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace
{
	constexpr int kConnectTimeoutMs = 3000;
	constexpr int kTlsHandshakeTimeoutMs = 5000;
	constexpr char kClientPreface[] = "WRC2TLS\0";
	constexpr char kServerOkPreface[] = "WRC2OK\0\0";
	constexpr char kServerBusyPreface[] = "WRC2BUSY";
	constexpr char kServerIncompatiblePreface[] = "WRC2BAD\0";
	constexpr qsizetype kPrefaceBytes = 8;
	constexpr qsizetype kMaximumTlsHandshakeBufferBytes = 256 * 1024;
	constexpr qsizetype kMaximumSecureRecordBufferBytes =
		KProtocolConstraints::kMaximumSignalingMessageBytes + 64 * 1024;
	constexpr qint64 kSocketWriteHighWaterBytes = 512 * 1024;
	constexpr int kMaximumPendingConnections = 4;
	constexpr int kListenBacklogSize = 8;

	QByteArray FixedPreface(const char *pData)
	{
		return QByteArray(pData, static_cast<int>(kPrefaceBytes));
	}
}

KTcpSignalingTransport::KTcpSignalingTransport(QObject *pParent)
	: KSignalingTransport(pParent)
	, m_pConnectTimeoutTimer(new QTimer(this))
	, m_pReadTimeoutTimer(new QTimer(this))
{
	m_pFallbackAdmissionController = new KAdmissionController();
	m_pAdmissionController = m_pFallbackAdmissionController;
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
	delete m_pFallbackAdmissionController;
}

void KTcpSignalingTransport::setAdmissionController(
	KAdmissionController *pController)
{
	m_pAdmissionController = pController != nullptr
		? pController : m_pFallbackAdmissionController;
}

bool KTcpSignalingTransport::setIdentityProvider(
	KDeviceIdentityProvider *pIdentityProvider,
	QString *pErrorMessage)
{
	if (pIdentityProvider == nullptr
		|| !pIdentityProvider->certificate().isValid())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("A valid Schannel device identity is required");
		return false;
	}
	m_pIdentityProvider = pIdentityProvider;
	return true;
}

bool KTcpSignalingTransport::startServer(quint16 nPort, QString *pErrorMessage)
{
	stop();
	if (m_pIdentityProvider == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Secure signaling identity is not configured");
		return false;
	}

	m_pServer = new QTcpServer(this);
	m_pServer->setProxy(QNetworkProxy::NoProxy);
	m_pServer->setMaxPendingConnections(kMaximumPendingConnections);
	m_pServer->setListenBacklogSize(kListenBacklogSize);
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

quint16 KTcpSignalingTransport::listeningPort() const
{
	return m_pServer != nullptr ? m_pServer->serverPort() : 0;
}

void KTcpSignalingTransport::connectToHost(const QString &strHost, quint16 nPort)
{
	stop();
	if (m_pIdentityProvider == nullptr)
	{
		emit outgoingConnectionFailed(QStringLiteral("Secure signaling identity is not configured"));
		return;
	}
	QTcpSocket *pSocket = new QTcpSocket(this);
	pSocket->setProxy(QNetworkProxy::NoProxy);
	setSocket(pSocket);
	m_bOutgoing = true;
	m_bOutgoingConnectionPending = true;
	m_connectElapsedTimer.start();
	m_pConnectTimeoutTimer->start(kConnectTimeoutMs);
	emit stateChanged(QStringLiteral("Connecting"));
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("signaling_connect_start"),
		QStringLiteral("host=%1 port=%2 timeoutMs=%3")
			.arg(strHost).arg(nPort).arg(kConnectTimeoutMs));
	pSocket->connectToHost(strHost, nPort);
}

void KTcpSignalingTransport::setServerBusyMessage(const QString &strMessage)
{
	Q_UNUSED(strMessage);
}

void KTcpSignalingTransport::disconnectPeer()
{
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	m_pConnectTimeoutTimer->stop();
	m_pReadTimeoutTimer->stop();
	m_connectElapsedTimer.invalidate();
	closeSocket(true);
	emit stateChanged(m_pServer != nullptr && m_pServer->isListening()
		? QStringLiteral("Listening") : QStringLiteral("Idle"));
}

void KTcpSignalingTransport::stop()
{
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	m_pConnectTimeoutTimer->stop();
	m_pReadTimeoutTimer->stop();
	m_connectElapsedTimer.invalidate();
	closeSocket(true);
	if (m_pServer != nullptr)
	{
		m_pServer->close();
		m_pServer->deleteLater();
		m_pServer = nullptr;
	}
	emit stateChanged(QStringLiteral("Idle"));
}

bool KTcpSignalingTransport::isConnected() const
{
	return m_pSocket != nullptr
		&& m_pSocket->state() == QAbstractSocket::ConnectedState;
}

bool KTcpSignalingTransport::exportKeyingMaterial(const QByteArray &label,
	const QByteArray &context,
	int nLength,
	QByteArray *pKeyingMaterial,
	QString *pErrorMessage)
{
	if (m_stage != SecureStage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("The secure signaling channel is not ready");
		return false;
	}
	return m_tlsEngine.exportKeyingMaterial(label, context, nLength,
		pKeyingMaterial, pErrorMessage);
}

void KTcpSignalingTransport::sendMessage(const QString &strMessage)
{
	if (!isConnected() || m_stage != SecureStage)
		return;
	QByteArray data = strMessage.toUtf8();
	if (data.size() > KProtocolConstraints::kMaximumSignalingMessageBytes)
	{
		emit signalingError(QStringLiteral("Outgoing signaling message exceeds size limit"));
		return;
	}
	data.append('\n');
	QList<QByteArray> records;
	QString strError;
	if (!m_tlsEngine.encrypt(data, &records, &strError))
	{
		rejectPeerData(strError, true);
		return;
	}
	for (const QByteArray &record : records)
	{
		if (!writeRaw(record, &strError))
		{
			rejectPeerData(strError);
			return;
		}
	}
}

void KTcpSignalingTransport::handleNewConnection()
{
	if (m_pServer == nullptr)
		return;
	while (m_pServer->hasPendingConnections())
	{
		QTcpSocket *pSocket = m_pServer->nextPendingConnection();
		if (pSocket == nullptr)
			return;
		pSocket->setProxy(QNetworkProxy::NoProxy);
		if (isSourceRateLimited(pSocket->peerAddress().toString()))
		{
			pSocket->write(FixedPreface(kServerBusyPreface));
			pSocket->flush();
			pSocket->disconnectFromHost();
			pSocket->deleteLater();
			continue;
		}
		if (m_bPeerBusy || isConnected())
		{
			pSocket->write(FixedPreface(kServerBusyPreface));
			pSocket->flush();
			pSocket->disconnectFromHost();
			pSocket->deleteLater();
			continue;
		}
		closeSocket();
		setSocket(pSocket);
		m_bOutgoing = false;
		m_bPeerBusy = true;
		m_stage = AwaitingClientPrefaceStage;
		m_pReadTimeoutTimer->start(kTlsHandshakeTimeoutMs);
		emit stateChanged(QStringLiteral("Securing"));
	}
}

void KTcpSignalingTransport::handleReadyRead()
{
	if (m_pSocket == nullptr)
		return;
	QString strError;
	while (m_pSocket != nullptr && m_pSocket->bytesAvailable() > 0)
	{
		if (!readAvailableData(&strError))
		{
			rejectPeerData(strError, true, true);
			return;
		}
		if (m_stage == AwaitingClientPrefaceStage
			|| m_stage == AwaitingServerPrefaceStage)
		{
			if (!processPreface(&strError))
			{
				const bool bPeerFailure = strError.contains(
					QStringLiteral("protocol version"));
				rejectPeerData(strError, true, bPeerFailure);
				return;
			}
		}
		else if (m_stage == TlsHandshakeStage && !processTls(&strError))
		{
			rejectPeerData(strError, true, true);
			return;
		}
		else if (m_stage == SecureStage && !processPlaintext(&strError))
		{
			rejectPeerData(strError, true, true);
			return;
		}
	}
}

void KTcpSignalingTransport::handleConnected()
{
	if (!m_bOutgoingConnectionPending)
		return;
	m_pConnectTimeoutTimer->stop();
	m_stage = AwaitingServerPrefaceStage;
	m_pReadTimeoutTimer->start(kTlsHandshakeTimeoutMs);
	QString strError;
	if (!writeRaw(FixedPreface(kClientPreface), &strError))
		rejectPeerData(strError);
}

void KTcpSignalingTransport::handleDisconnected()
{
	m_pReadTimeoutTimer->stop();
	const bool bWasPending = m_bOutgoingConnectionPending;
	const bool bHandshakeTruncated = m_stage == AwaitingClientPrefaceStage
		|| m_stage == AwaitingServerPrefaceStage
		|| m_stage == TlsHandshakeStage;
	const QString strSourceAddress = m_pSocket != nullptr
		? m_pSocket->peerAddress().toString() : QString();
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	m_stage = IdleStage;
	if (bHandshakeTruncated)
	{
		if (!m_bOutgoing)
			recordSourceFailure(strSourceAddress);
		KSessionError error;
		error.domain = SecuritySessionErrorDomain;
		error.code = ConnectionFailedSessionErrorCode;
		error.stage = m_bOutgoing ? ConnectingSessionErrorStage
			: ListeningSessionErrorStage;
		error.bRetryable = true;
		error.strTechnicalMessage = QStringLiteral(
			"Secure signaling handshake was truncated by the peer");
		emit tlsHandshakeFailed(error);
	}
	if (bWasPending)
		emit outgoingConnectionFailed(QStringLiteral("Secure signaling connection closed"));
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
	if (m_bOutgoingConnectionPending)
		failOutgoingConnection(QStringLiteral("timeout"),
			QStringLiteral("Signaling connection timed out after %1 ms")
				.arg(kConnectTimeoutMs));
}

void KTcpSignalingTransport::handleReadTimeout()
{
	if (isConnected())
		rejectPeerData(QStringLiteral("Secure signaling handshake timed out"), true,
			!m_bOutgoing);
}

void KTcpSignalingTransport::setSocket(QTcpSocket *pSocket)
{
	m_encryptedBuffer.clear();
	m_plaintextBuffer.clear();
	m_tlsEngine.clear();
	m_stage = IdleStage;
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

void KTcpSignalingTransport::closeSocket(bool bSendCloseNotify)
{
	m_pReadTimeoutTimer->stop();
	if (bSendCloseNotify && m_stage == SecureStage && m_pSocket != nullptr
		&& m_pSocket->state() == QAbstractSocket::ConnectedState)
	{
		QList<QByteArray> closeRecords;
		QString strCloseError;
		if (m_tlsEngine.shutdown(&closeRecords, &strCloseError))
		{
			for (const QByteArray &record : closeRecords)
			{
				if (!writeRaw(record, &strCloseError))
					break;
			}
		}
		if (!strCloseError.isEmpty())
		{
			KSessionTraceLogger::write(m_bOutgoing
					? QStringLiteral("controller") : QStringLiteral("controlled"),
				QStringLiteral("tls"), QStringLiteral("close_notify_failed"), -1,
				QStringLiteral("error=%1").arg(strCloseError));
		}
	}
	m_tlsEngine.clear();
	m_encryptedBuffer.clear();
	m_plaintextBuffer.clear();
	m_stage = IdleStage;
	if (m_pSocket == nullptr)
		return;
	m_pSocket->disconnect(this);
	m_pSocket->disconnectFromHost();
	m_pSocket->deleteLater();
	m_pSocket = nullptr;
}

bool KTcpSignalingTransport::writeRaw(const QByteArray &data,
	QString *pErrorMessage)
{
	if (m_pSocket == nullptr || data.isEmpty())
		return false;
	if (data.size() > kSocketWriteHighWaterBytes
		|| m_pSocket->bytesToWrite()
			> kSocketWriteHighWaterBytes - data.size())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Secure signaling send buffer exceeds size limit");
		return false;
	}
	const qint64 nAcceptedBytes = m_pSocket->write(data);
	if (nAcceptedBytes != data.size())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to queue complete secure signaling data");
		return false;
	}
	m_pSocket->flush();
	return true;
}

qsizetype KTcpSignalingTransport::maximumEncryptedBufferBytes() const
{
	if (m_stage == AwaitingClientPrefaceStage
		|| m_stage == AwaitingServerPrefaceStage)
	{
		return kPrefaceBytes;
	}
	if (m_stage == TlsHandshakeStage)
		return kMaximumTlsHandshakeBufferBytes;
	if (m_stage == SecureStage)
		return kMaximumSecureRecordBufferBytes;
	return 0;
}

bool KTcpSignalingTransport::readAvailableData(QString *pErrorMessage)
{
	const qsizetype nMaximumBytes = maximumEncryptedBufferBytes();
	const qsizetype nRemainingBytes = nMaximumBytes - m_encryptedBuffer.size();
	if (nMaximumBytes <= 0 || nRemainingBytes <= 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Secure signaling receive buffer exceeds size limit");
		return false;
	}
	const qint64 nReadBytes = qMin<qint64>(m_pSocket->bytesAvailable(),
		static_cast<qint64>(nRemainingBytes));
	const QByteArray data = m_pSocket->read(nReadBytes);
	if (data.isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to read secure signaling data");
		return false;
	}
	m_encryptedBuffer.append(data);
	return true;
}

bool KTcpSignalingTransport::beginTls(bool bServer, QString *pErrorMessage)
{
	void *pCertificate = m_pIdentityProvider->duplicateNativeCertificate(pErrorMessage);
	if (pCertificate == nullptr
		|| !m_tlsEngine.initialize(bServer, pCertificate, pErrorMessage))
	{
		return false;
	}
	m_stage = TlsHandshakeStage;
	QList<QByteArray> records;
	if (!m_tlsEngine.start(&records, pErrorMessage))
		return false;
	for (const QByteArray &record : records)
	{
		if (!writeRaw(record, pErrorMessage))
			return false;
	}
	return true;
}

bool KTcpSignalingTransport::processPreface(QString *pErrorMessage)
{
	if (m_encryptedBuffer.size() < kPrefaceBytes)
		return true;
	const QByteArray preface = m_encryptedBuffer.left(kPrefaceBytes);
	m_encryptedBuffer.remove(0, kPrefaceBytes);
	if (m_stage == AwaitingClientPrefaceStage)
	{
		if (preface != FixedPreface(kClientPreface))
		{
			writeRaw(FixedPreface(kServerIncompatiblePreface));
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Secure signaling protocol version is incompatible");
			return false;
		}
		if (!writeRaw(FixedPreface(kServerOkPreface), pErrorMessage))
			return false;
		if (!beginTls(true, pErrorMessage))
			return false;
	}
	else
	{
		if (preface == FixedPreface(kServerBusyPreface))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("The controlled device is busy");
			return false;
		}
		if (preface != FixedPreface(kServerOkPreface))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Secure signaling protocol version is incompatible");
			return false;
		}
		if (!beginTls(false, pErrorMessage))
			return false;
	}
	if (!m_encryptedBuffer.isEmpty())
		return processTls(pErrorMessage);
	return true;
}

bool KTcpSignalingTransport::processTls(QString *pErrorMessage)
{
	QList<QByteArray> records;
	bool bCompleted = false;
	if (!m_tlsEngine.continueHandshake(&m_encryptedBuffer,
		&records, &bCompleted, pErrorMessage))
	{
		return false;
	}
	for (const QByteArray &record : records)
	{
		if (!writeRaw(record, pErrorMessage))
			return false;
	}
	if (!bCompleted)
		return true;
	completeSecureConnection();
	if (!m_encryptedBuffer.isEmpty())
		return processPlaintext(pErrorMessage);
	return true;
}

bool KTcpSignalingTransport::processPlaintext(QString *pErrorMessage)
{
	QList<QByteArray> plainTexts;
	bool bClosed = false;
	if (!m_tlsEngine.decrypt(&m_encryptedBuffer, &plainTexts,
		&bClosed, pErrorMessage))
	{
		return false;
	}
	for (const QByteArray &plainText : plainTexts)
		m_plaintextBuffer.append(plainText);
	if (m_plaintextBuffer.size() > KProtocolConstraints::kMaximumSignalingMessageBytes + 1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Signaling receive buffer exceeds size limit");
		return false;
	}
	for (;;)
	{
		const qsizetype nLineEnd = m_plaintextBuffer.indexOf('\n');
		if (nLineEnd < 0)
			break;
		const QByteArray line = m_plaintextBuffer.left(nLineEnd).trimmed();
		m_plaintextBuffer.remove(0, nLineEnd + 1);
		if (!line.isEmpty())
		{
			emit messageReceived(QString::fromUtf8(line));
			if (m_pSocket == nullptr || m_stage != SecureStage)
				return true;
		}
	}
	// Schannel can return final application data and close_notify together.
	// Deliver the plaintext first so a terminal protocol response is not
	// misreported as an unexplained signaling loss.
	if (bClosed)
	{
		KSessionTraceLogger::write(m_bOutgoing
				? QStringLiteral("controller") : QStringLiteral("controlled"),
			QStringLiteral("tls"), QStringLiteral("close_notify_received"), -1,
			QString());
		m_bOutgoingConnectionPending = false;
		m_bPeerBusy = false;
		closeSocket();
		emit stateChanged(m_pServer != nullptr && m_pServer->isListening()
			? QStringLiteral("Listening") : QStringLiteral("Disconnected"));
		emit connectionLost();
	}
	return true;
}

void KTcpSignalingTransport::completeSecureConnection()
{
	KTlsPeerIdentity peer;
	QString strError;
	const QString strSource = m_pSocket != nullptr
		? m_pSocket->peerAddress().toString() : QString();
	if (!m_tlsEngine.peerIdentity(strSource, &peer, &strError))
	{
		rejectPeerData(strError, true);
		return;
	}
	m_stage = SecureStage;
	m_pReadTimeoutTimer->stop();
	KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
			: QStringLiteral("controlled"),
		QStringLiteral("tls"), QStringLiteral("secure_channel"), -1,
		QStringLiteral("protocol=%1 cipher=%2 fingerprint=%3")
			.arg(peer.strTlsProtocol, peer.strCipherSuite,
				QString::fromLatin1(peer.spkiSha256.toHex().left(12))));
	emit secureChannelEstablished(peer);
	emit stateChanged(QStringLiteral("Connected"));
	if (m_bOutgoing)
	{
		m_bOutgoingConnectionPending = false;
		const qint64 nCostMs = m_connectElapsedTimer.elapsed();
		m_connectElapsedTimer.invalidate();
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("signaling_connect_success"),
			QStringLiteral("costMs=%1 secure=1").arg(nCostMs));
		emit outgoingConnectionEstablished();
	}
	else if (m_pSocket != nullptr)
	{
		emit incomingConnectionEstablished(m_pSocket->peerAddress().toString(),
			m_pSocket->peerPort());
	}
}

void KTcpSignalingTransport::rejectPeerData(const QString &strMessage,
	bool bTlsFailure,
	bool bCountSourceFailure)
{
	const QString strSourceAddress = m_pSocket != nullptr
		? m_pSocket->peerAddress().toString() : QString();
	if (!m_bOutgoing && bCountSourceFailure)
		recordSourceFailure(strSourceAddress);
	if (bTlsFailure)
	{
		KSessionTraceLogger::write(m_bOutgoing ? QStringLiteral("controller")
				: QStringLiteral("controlled"),
			QStringLiteral("tls"), QStringLiteral("handshake_failed"), -1,
			QStringLiteral("error=%1").arg(strMessage));
	}
	m_pReadTimeoutTimer->stop();
	const bool bOutgoingPending = m_bOutgoingConnectionPending;
	m_bOutgoingConnectionPending = false;
	m_bPeerBusy = false;
	closeSocket();
	if (bTlsFailure)
	{
		KSessionError error;
		error.domain = SecuritySessionErrorDomain;
		error.code = ConnectionFailedSessionErrorCode;
		error.stage = m_bOutgoing ? ConnectingSessionErrorStage
			: ListeningSessionErrorStage;
		error.bRetryable = true;
		error.strTechnicalMessage = strMessage;
		emit tlsHandshakeFailed(error);
	}
	if (bOutgoingPending)
		emit outgoingConnectionFailed(strMessage);
	else
		emit signalingError(strMessage);
	emit stateChanged(m_pServer != nullptr && m_pServer->isListening()
		? QStringLiteral("Listening") : QStringLiteral("ConnectionFailed"));
	emit connectionLost();
}

bool KTcpSignalingTransport::isSourceRateLimited(
	const QString &strSourceAddress)
{
	return m_pAdmissionController != nullptr
		&& m_pAdmissionController->isRateLimited(strSourceAddress);
}

void KTcpSignalingTransport::recordSourceFailure(
	const QString &strSourceAddress)
{
	if (m_pAdmissionController != nullptr)
		m_pAdmissionController->recordPeerFailure(strSourceAddress);
}

void KTcpSignalingTransport::failOutgoingConnection(
	const QString &strReason,
	const QString &strMessage)
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
			.arg(nCostMs).arg(strReason, strMessage));
	closeSocket();
	emit stateChanged(QStringLiteral("ConnectionFailed"));
	emit outgoingConnectionFailed(strMessage);
}
