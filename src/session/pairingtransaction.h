#ifndef _WINREMOTECONTROL_SESSION_PAIRINGTRANSACTION_H_
#define _WINREMOTECONTROL_SESSION_PAIRINGTRANSACTION_H_

#include "core/security/permissionscope.h"

#include <QtCore/QString>

enum KPairingTransactionState
{
	InactivePairingTransactionState,
	ExchangingHelloPairingTransactionState,
	AwaitingDecisionPairingTransactionState,
	PreparingPairingTransactionState,
	CommittingPairingTransactionState,
	CompletedPairingTransactionState,
	FailedPairingTransactionState
};

class KPairingTransaction
{
public:
	void begin(const QString &strRequestId, quint64 nGeneration);
	void reset();
	bool matches(const QString &strRequestId, quint64 nGeneration) const;

	bool markHelloSent();
	bool markHelloReceived();
	bool markLocalDecision(KPermissionScopes permissions);
	bool markRemoteDecision(KPermissionScopes permissions);
	bool markLocalPrepared();
	bool markRemotePrepared();
	bool markLocalCommitted();
	bool markRemoteCommitted();
	bool markCompleted();
	void markFailed();

	bool helloSent() const;
	bool helloReceived() const;
	bool decisionsComplete() const;
	bool localPrepared() const;
	bool preparedComplete() const;
	bool localCommitted() const;
	bool committedComplete() const;
	KPermissionScopes localPermissions() const;
	KPermissionScopes remotePermissions() const;
	KPairingTransactionState state() const;
	QString requestId() const;
	quint64 generation() const;

private:
	enum ProgressFlag : quint32
	{
		HelloSentProgress = 1u << 0,
		HelloReceivedProgress = 1u << 1,
		LocalDecisionProgress = 1u << 2,
		RemoteDecisionProgress = 1u << 3,
		LocalPreparedProgress = 1u << 4,
		RemotePreparedProgress = 1u << 5,
		LocalCommittedProgress = 1u << 6,
		RemoteCommittedProgress = 1u << 7
	};

	bool markProgress(ProgressFlag flag);
	bool hasProgress(ProgressFlag flag) const;
	void updateState();

	QString m_strRequestId;
	quint64 m_nGeneration = 0;
	quint32 m_nProgress = 0;
	KPermissionScopes m_localPermissions;
	KPermissionScopes m_remotePermissions;
	KPairingTransactionState m_state = InactivePairingTransactionState;
};

#endif // _WINREMOTECONTROL_SESSION_PAIRINGTRANSACTION_H_
