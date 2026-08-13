#include "session/accesssessionflow.h"

#include "common/sessiontracelogger.h"
#include "core/transport/signalingtransport.h"
#include "session/accessapprovalcontroller.h"

#include <QtCore/QDateTime>

namespace
{
	constexpr int kInitialApprovalResponseTimeoutMs = 5000;
	constexpr int kApprovalResponseGraceMs = 5000;
}

KAccessSessionFlow::KAccessSessionFlow(KSignalingTransport *pTransport,
	QObject *pParent)
	: QObject(pParent)
	, m_pTransport(pTransport)
	, m_pApprovalController(new KAccessApprovalController(this))
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
	connect(m_pApprovalController, &KAccessApprovalController::timedOut,
		this, &KAccessSessionFlow::handleApprovalTimeout);
}

void KAccessSessionFlow::setApplicationSettings(
	const KApplicationSettings &settings)
{
	m_settings = settings;
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
	clearApproval(QStringLiteral("new_connection"));
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
	clearApproval(QStringLiteral("stopped"));
	m_bConnected = false;
	m_pTransport->stop();
}

void KAccessSessionFlow::beginOutgoing(
	quint64 nGeneration,
	const QString &strDeviceName)
{
	const KAccessApprovalRequest request = m_pApprovalController->beginOutgoing(
		nGeneration, kInitialApprovalResponseTimeoutMs);
	KAccessMessage message;
	message.type = RequestAccessMessageType;
	message.strRequestId = request.strRequestId;
	message.strDeviceName = strDeviceName;
	sendAccessMessage(message);
	KSessionTraceLogger::write(QStringLiteral("controller"), QStringLiteral("access"),
		QStringLiteral("request_sent"), -1,
		QStringLiteral("requestId=%1 generation=%2")
			.arg(request.strRequestId).arg(request.nGeneration));
}

void KAccessSessionFlow::beginIncoming(
	const QString &strSourceAddress,
	quint64 nGeneration)
{
	m_pApprovalController->beginIncoming(strSourceAddress, nGeneration,
		kInitialApprovalResponseTimeoutMs);
}

bool KAccessSessionFlow::handleAccessMessage(
	const KAccessMessage &message,
	quint64 nGeneration)
{
	const KAccessApprovalRequest current = m_pApprovalController->request();
	if (current.side == IncomingAccessApprovalSide)
	{
		if (message.type == RejectedAccessMessageType
			&& m_pApprovalController->matches(message.strRequestId, nGeneration))
		{
			rejectIncoming(QStringLiteral("cancelled"), false);
			return true;
		}
		if (message.type != RequestAccessMessageType
			|| m_pApprovalController->hasRequestId())
		{
			return true;
		}
		if (!m_pApprovalController->receiveIncomingRequest(message.strRequestId,
			message.strDeviceName, m_settings.nApprovalTimeoutSeconds * 1000))
		{
			return false;
		}
		const KAccessApprovalRequest &request = m_pApprovalController->request();
		emit incomingAccessObserved(request.strDeviceName, request.strSourceAddress);
		switch (KAccessApprovalController::incomingDecision(m_settings))
		{
		case DisabledIncomingAccessDecision:
			rejectIncoming(QStringLiteral("remote_access_disabled"), true);
			return true;
		case DenyIncomingAccessDecision:
			rejectIncoming(QStringLiteral("user_rejected"), true);
			return true;
		case AcceptIncomingAccessDecision:
			acceptIncoming();
			return true;
		case AskIncomingAccessDecision:
		default:
			break;
		}
		KAccessMessage pending;
		pending.type = PendingAccessMessageType;
		pending.strRequestId = request.strRequestId;
		pending.nTimeoutSeconds = m_settings.nApprovalTimeoutSeconds;
		sendAccessMessage(pending);
		const qint64 nExpiresAtMs = QDateTime::currentMSecsSinceEpoch()
			+ m_settings.nApprovalTimeoutSeconds * 1000LL;
		emit incomingAccessRequest(request.strRequestId, request.strDeviceName,
			request.strSourceAddress, nExpiresAtMs);
		return true;
	}

	if (!m_pApprovalController->matches(message.strRequestId, nGeneration))
		return true;
	if (message.type == PendingAccessMessageType)
	{
		m_pApprovalController->extendOutgoingTimeout(message.strRequestId,
			message.nTimeoutSeconds * 1000 + kApprovalResponseGraceMs);
		return true;
	}
	if (message.type == AcceptedAccessMessageType)
	{
		m_pApprovalController->stopTimeout();
		clearApproval(QStringLiteral("accepted"));
		emit outgoingAccessAccepted();
		return true;
	}
	if (message.type == RejectedAccessMessageType)
	{
		const QString strReason = message.strReason;
		clearApproval(strReason);
		emit outgoingAccessRejected(strReason);
		return true;
	}
	return true;
}

void KAccessSessionFlow::respondIncoming(
	const QString &strRequestId,
	bool bAccepted)
{
	if (!m_pApprovalController->matches(strRequestId,
		m_pApprovalController->request().nGeneration)
		|| m_pApprovalController->request().side != IncomingAccessApprovalSide)
	{
		return;
	}
	if (bAccepted)
		acceptIncoming();
	else
		rejectIncoming(QStringLiteral("user_rejected"), true);
}

void KAccessSessionFlow::rejectIncoming(
	const QString &strReason,
	bool bNotifyRemote)
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side != IncomingAccessApprovalSide)
		return;
	if (bNotifyRemote && !request.strRequestId.isEmpty())
	{
		KAccessMessage rejected;
		rejected.type = RejectedAccessMessageType;
		rejected.strRequestId = request.strRequestId;
		rejected.strReason = strReason;
		sendAccessMessage(rejected);
	}
	clearApproval(strReason);
	emit incomingAccessRejected(strReason);
}

void KAccessSessionFlow::cancelApproval(
	const QString &strReason,
	bool bNotifyRemote,
	bool bEmitOutcome)
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side == NoAccessApprovalSide)
		return;
	if (bNotifyRemote && !request.strRequestId.isEmpty())
	{
		KAccessMessage rejected;
		rejected.type = RejectedAccessMessageType;
		rejected.strRequestId = request.strRequestId;
		rejected.strReason = strReason;
		sendAccessMessage(rejected);
	}
	clearApproval(strReason);
	if (!bEmitOutcome)
		return;
	if (request.side == IncomingAccessApprovalSide)
		emit incomingAccessRejected(strReason);
	else
		emit outgoingAccessRejected(strReason);
}

void KAccessSessionFlow::clearApproval(const QString &strReason)
{
	const KAccessApprovalRequest request = m_pApprovalController->clear();
	if (request.side == IncomingAccessApprovalSide && !request.strRequestId.isEmpty())
		emit incomingAccessRequestCleared(request.strRequestId, strReason);
}

bool KAccessSessionFlow::hasApproval() const
{
	return m_pApprovalController->request().side != NoAccessApprovalSide;
}

void KAccessSessionFlow::sendSignalingMessage(const QString &strMessage)
{
	m_pTransport->sendMessage(strMessage);
}

void KAccessSessionFlow::setConnected(bool bConnected) { m_bConnected = bConnected; }
bool KAccessSessionFlow::isConnected() const { return m_bConnected; }

bool KAccessSessionFlow::matchesEndpoint(const QString &strHost, quint16 nPort) const
{
	return m_strLastHost.compare(strHost, Qt::CaseInsensitive) == 0
		&& m_nLastPort == nPort;
}

bool KAccessSessionFlow::hasLastEndpoint() const
{
	return !m_strLastHost.isEmpty() && m_nLastPort != 0;
}

QString KAccessSessionFlow::lastHost() const { return m_strLastHost; }
quint16 KAccessSessionFlow::lastPort() const { return m_nLastPort; }
quint16 KAccessSessionFlow::listeningPort() const { return m_nListeningPort; }

void KAccessSessionFlow::clearLastEndpoint()
{
	m_strLastHost.clear();
	m_nLastPort = 0;
}

void KAccessSessionFlow::sendAccessMessage(const KAccessMessage &message)
{
	m_pTransport->sendMessage(KAccessMessageCodec::encode(message));
}

void KAccessSessionFlow::acceptIncoming()
{
	const KAccessApprovalRequest request = m_pApprovalController->request();
	if (request.side != IncomingAccessApprovalSide || request.strRequestId.isEmpty())
		return;
	KAccessMessage accepted;
	accepted.type = AcceptedAccessMessageType;
	accepted.strRequestId = request.strRequestId;
	sendAccessMessage(accepted);
	clearApproval(QStringLiteral("accepted"));
	emit incomingAccessAccepted();
}

void KAccessSessionFlow::handleApprovalTimeout(
	const QString &strRequestId,
	quint64 nGeneration)
{
	if (!m_pApprovalController->matches(strRequestId, nGeneration))
		return;
	if (m_pApprovalController->request().side == IncomingAccessApprovalSide)
		rejectIncoming(QStringLiteral("timeout"), true);
	else
	{
		clearApproval(QStringLiteral("timeout"));
		emit outgoingAccessRejected(QStringLiteral("timeout"));
	}
}
