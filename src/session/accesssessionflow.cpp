#include "session/accesssessionflow.h"

#include "core/transport/signalingtransport.h"

KAccessSessionFlow::KAccessSessionFlow(KSignalingTransport *pTransport,
	QObject *pParent)
	: QObject(pParent)
	, m_pTransport(pTransport)
{
	Q_ASSERT(m_pTransport != nullptr);
	KAccessMessage busy;
	busy.type = ServerBusyAccessMessageType;
	m_pTransport->setServerBusyMessage(KAccessMessageCodec::encode(busy));
	connect(m_pTransport, &KSignalingTransport::messageReceived,
		this, &KAccessSessionFlow::messageReceived);
	connect(m_pTransport, &KSignalingTransport::stateChanged,
		this, &KAccessSessionFlow::stateChanged);
	connect(m_pTransport, &KSignalingTransport::signalingError,
		this, &KAccessSessionFlow::signalingError);
	connect(m_pTransport, &KSignalingTransport::outgoingConnectionEstablished,
		this, &KAccessSessionFlow::outgoingConnectionEstablished);
	connect(m_pTransport, &KSignalingTransport::outgoingConnectionFailed,
		this, &KAccessSessionFlow::outgoingConnectionFailed);
	connect(m_pTransport, &KSignalingTransport::incomingConnectionEstablished,
		this, &KAccessSessionFlow::incomingConnectionEstablished);
	connect(m_pTransport, &KSignalingTransport::connectionLost,
		this, &KAccessSessionFlow::connectionLost);
}

bool KAccessSessionFlow::startListening(quint16 nPort, QString *pErrorMessage)
{
	m_bConnected = false;
	if (!m_pTransport->startServer(nPort, pErrorMessage))
		return false;
	m_nListeningPort = nPort;
	return true;
}

void KAccessSessionFlow::connectToHost(const QString &strHost, quint16 nPort)
{
	m_strLastHost = strHost;
	m_nLastPort = nPort;
	m_bConnected = false;
	m_pTransport->stop();
	m_pTransport->connectToHost(strHost, nPort);
}

void KAccessSessionFlow::disconnectPeer()
{
	m_bConnected = false;
	m_pTransport->disconnectPeer();
}

void KAccessSessionFlow::stop()
{
	m_bConnected = false;
	m_pTransport->stop();
}

void KAccessSessionFlow::sendAccessMessage(const KAccessMessage &message)
{
	m_pTransport->sendMessage(KAccessMessageCodec::encode(message));
}

void KAccessSessionFlow::sendSignalingMessage(const QString &strMessage)
{
	m_pTransport->sendMessage(strMessage);
}

void KAccessSessionFlow::setConnected(bool bConnected)
{
	m_bConnected = bConnected;
}

bool KAccessSessionFlow::isConnected() const
{
	return m_bConnected;
}

bool KAccessSessionFlow::matchesEndpoint(const QString &strHost, quint16 nPort) const
{
	return m_strLastHost.compare(strHost, Qt::CaseInsensitive) == 0
		&& m_nLastPort == nPort;
}

bool KAccessSessionFlow::hasLastEndpoint() const
{
	return !m_strLastHost.isEmpty() && m_nLastPort != 0;
}

QString KAccessSessionFlow::lastHost() const
{
	return m_strLastHost;
}

quint16 KAccessSessionFlow::lastPort() const
{
	return m_nLastPort;
}

quint16 KAccessSessionFlow::listeningPort() const
{
	return m_nListeningPort;
}

void KAccessSessionFlow::clearLastEndpoint()
{
	m_strLastHost.clear();
	m_nLastPort = 0;
}
