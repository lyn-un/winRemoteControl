#include "session/pairingtransaction.h"

#include <QtCore/QUuid>

void KPairingTransaction::begin(const QString &strRequestId,
	quint64 nGeneration)
{
	reset();
	if (QUuid(strRequestId).isNull() || nGeneration == 0)
	{
		m_state = FailedPairingTransactionState;
		return;
	}
	m_strRequestId = strRequestId;
	m_nGeneration = nGeneration;
	m_state = ExchangingHelloPairingTransactionState;
}

void KPairingTransaction::reset()
{
	m_strRequestId.clear();
	m_nGeneration = 0;
	m_nProgress = 0;
	m_localPermissions = KPermissionScopes();
	m_remotePermissions = KPermissionScopes();
	m_state = InactivePairingTransactionState;
}

bool KPairingTransaction::matches(const QString &strRequestId,
	quint64 nGeneration) const
{
	return m_state != InactivePairingTransactionState
		&& m_state != FailedPairingTransactionState
		&& strRequestId == m_strRequestId
		&& nGeneration == m_nGeneration;
}

bool KPairingTransaction::markHelloSent()
{
	return markProgress(HelloSentProgress);
}

bool KPairingTransaction::markHelloReceived()
{
	return markProgress(HelloReceivedProgress);
}

bool KPairingTransaction::markLocalDecision(KPermissionScopes permissions)
{
	if (hasProgress(LocalDecisionProgress))
		return false;
	m_localPermissions = permissions;
	return markProgress(LocalDecisionProgress);
}

bool KPairingTransaction::markRemoteDecision(KPermissionScopes permissions)
{
	if (hasProgress(RemoteDecisionProgress))
		return false;
	m_remotePermissions = permissions;
	return markProgress(RemoteDecisionProgress);
}

bool KPairingTransaction::markLocalPrepared()
{
	return decisionsComplete() && markProgress(LocalPreparedProgress);
}

bool KPairingTransaction::markRemotePrepared()
{
	return decisionsComplete() && markProgress(RemotePreparedProgress);
}

bool KPairingTransaction::markLocalCommitted()
{
	return preparedComplete() && markProgress(LocalCommittedProgress);
}

bool KPairingTransaction::markRemoteCommitted()
{
	return preparedComplete() && markProgress(RemoteCommittedProgress);
}

bool KPairingTransaction::markCompleted()
{
	if (!committedComplete() || m_state == CompletedPairingTransactionState)
		return false;
	m_state = CompletedPairingTransactionState;
	return true;
}

void KPairingTransaction::markFailed()
{
	if (m_state != CompletedPairingTransactionState)
		m_state = FailedPairingTransactionState;
}

bool KPairingTransaction::helloSent() const
{
	return hasProgress(HelloSentProgress);
}

bool KPairingTransaction::helloReceived() const
{
	return hasProgress(HelloReceivedProgress);
}

bool KPairingTransaction::decisionsComplete() const
{
	return hasProgress(LocalDecisionProgress)
		&& hasProgress(RemoteDecisionProgress);
}

bool KPairingTransaction::localPrepared() const
{
	return hasProgress(LocalPreparedProgress);
}

bool KPairingTransaction::preparedComplete() const
{
	return hasProgress(LocalPreparedProgress)
		&& hasProgress(RemotePreparedProgress);
}

bool KPairingTransaction::localCommitted() const
{
	return hasProgress(LocalCommittedProgress);
}

bool KPairingTransaction::committedComplete() const
{
	return hasProgress(LocalCommittedProgress)
		&& hasProgress(RemoteCommittedProgress);
}

KPermissionScopes KPairingTransaction::localPermissions() const
{
	return m_localPermissions;
}

KPermissionScopes KPairingTransaction::remotePermissions() const
{
	return m_remotePermissions;
}

KPairingTransactionState KPairingTransaction::state() const
{
	return m_state;
}

bool KPairingTransaction::isActive() const
{
	return m_state != InactivePairingTransactionState
		&& !isTerminal();
}

bool KPairingTransaction::isTerminal() const
{
	return m_state == CompletedPairingTransactionState
		|| m_state == FailedPairingTransactionState;
}

QString KPairingTransaction::requestId() const
{
	return m_strRequestId;
}

quint64 KPairingTransaction::generation() const
{
	return m_nGeneration;
}

bool KPairingTransaction::markProgress(ProgressFlag flag)
{
	if (m_state == InactivePairingTransactionState
		|| m_state == CompletedPairingTransactionState
		|| m_state == FailedPairingTransactionState
		|| hasProgress(flag))
	{
		return false;
	}
	m_nProgress |= flag;
	updateState();
	return true;
}

bool KPairingTransaction::hasProgress(ProgressFlag flag) const
{
	return (m_nProgress & static_cast<quint32>(flag)) != 0;
}

void KPairingTransaction::updateState()
{
	if (committedComplete())
		m_state = CommittingPairingTransactionState;
	else if (preparedComplete() || localCommitted())
		m_state = CommittingPairingTransactionState;
	else if (decisionsComplete() || localPrepared())
		m_state = PreparingPairingTransactionState;
	else if (helloSent() && helloReceived())
		m_state = AwaitingDecisionPairingTransactionState;
	else
		m_state = ExchangingHelloPairingTransactionState;
}
