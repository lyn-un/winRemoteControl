#include "session/accessapprovalcontroller.h"

#include <QtCore/QTimer>
#include <QtCore/QUuid>

KAccessApprovalController::KAccessApprovalController(QObject *pParent)
	: QObject(pParent)
	, m_pTimer(new QTimer(this))
{
	m_pTimer->setSingleShot(true);
	connect(m_pTimer, &QTimer::timeout, this,
		[this]() { emit timedOut(m_request.strRequestId, m_request.nGeneration); });
}

KAccessApprovalRequest KAccessApprovalController::beginOutgoing(
	quint64 nGeneration,
	int nTimeoutMs)
{
	clear();
	m_request.side = OutgoingAccessApprovalSide;
	m_request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_request.nGeneration = nGeneration;
	startTimeout(nTimeoutMs);
	return m_request;
}

void KAccessApprovalController::beginIncoming(const QString &strSourceAddress,
	quint64 nGeneration,
	int nTimeoutMs)
{
	clear();
	m_request.side = IncomingAccessApprovalSide;
	m_request.strSourceAddress = strSourceAddress;
	m_request.nGeneration = nGeneration;
	startTimeout(nTimeoutMs);
}

bool KAccessApprovalController::receiveIncomingRequest(const QString &strRequestId,
	const QString &strDeviceName,
	int nTimeoutMs)
{
	if (m_request.side != IncomingAccessApprovalSide
		|| !m_request.strRequestId.isEmpty()
		|| strRequestId.isEmpty())
	{
		return false;
	}
	m_request.strRequestId = strRequestId;
	m_request.strDeviceName = strDeviceName;
	startTimeout(nTimeoutMs);
	return true;
}

bool KAccessApprovalController::extendOutgoingTimeout(const QString &strRequestId,
	int nTimeoutMs)
{
	if (m_request.side != OutgoingAccessApprovalSide
		|| strRequestId != m_request.strRequestId)
	{
		return false;
	}
	startTimeout(nTimeoutMs);
	return true;
}

bool KAccessApprovalController::matches(const QString &strRequestId,
	quint64 nGeneration) const
{
	return !strRequestId.isEmpty()
		&& strRequestId == m_request.strRequestId
		&& nGeneration == m_request.nGeneration;
}

bool KAccessApprovalController::hasRequestId() const
{
	return !m_request.strRequestId.isEmpty();
}

const KAccessApprovalRequest &KAccessApprovalController::request() const
{
	return m_request;
}

void KAccessApprovalController::stopTimeout()
{
	m_pTimer->stop();
}

KAccessApprovalRequest KAccessApprovalController::clear()
{
	m_pTimer->stop();
	const KAccessApprovalRequest previous = m_request;
	m_request = KAccessApprovalRequest();
	return previous;
}

KIncomingAccessDecision KAccessApprovalController::incomingDecision(
	const KApplicationSettings &settings)
{
	if (!settings.bRemoteAccessEnabled)
		return DisabledIncomingAccessDecision;
	if (settings.approvalMode == DenyRemoteApprovalMode)
		return DenyIncomingAccessDecision;
	if (settings.approvalMode == AutoAcceptRemoteApprovalMode)
		return AcceptIncomingAccessDecision;
	return AskIncomingAccessDecision;
}

void KAccessApprovalController::startTimeout(int nTimeoutMs)
{
	m_pTimer->start(qMax(1, nTimeoutMs));
}
