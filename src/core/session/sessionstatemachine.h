#ifndef _WINREMOTECONTROL_CORE_SESSION_SESSIONSTATEMACHINE_H_
#define _WINREMOTECONTROL_CORE_SESSION_SESSIONSTATEMACHINE_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

enum KSessionRole
{
	ControllerSessionRole,
	ControlledSessionRole
};

enum KSessionState
{
	IdleSessionState,
	ListeningSessionState,
	ConnectingSessionState,
	AwaitingApprovalSessionState,
	NegotiatingSessionState,
	ConnectedSessionState,
	StreamingSessionState,
	ReconnectingSessionState,
	StoppingSessionState
};

enum KSessionEndReason
{
	LocalDisconnectSessionEndReason,
	RoleChangedSessionEndReason,
	RestartListenerSessionEndReason,
	NewConnectionSessionEndReason,
	ControlledUserStopSessionEndReason,
	CaptureFailedSessionEndReason,
	InputChannelClosedSessionEndReason,
	SessionChannelClosedSessionEndReason,
	ConnectFailedSessionEndReason,
	SignalingLostSessionEndReason,
	PeerTerminatedSessionEndReason,
	DisconnectTimeoutSessionEndReason,
	RemoteStopSessionEndReason
};

class KSessionStateMachine
{
public:
	KSessionRole role() const;
	KSessionState state() const;
	quint64 generation() const;

	bool setRole(KSessionRole role);
	bool beginListening();
	bool beginConnecting();
	bool beginAwaitingApproval();
	bool approveConnection();
	bool rejectConnection();
	bool markConnected();
	bool beginStreaming();
	bool stopStreaming();
	bool beginReconnecting();
	bool restore();
	bool beginStopping();
	void finish(bool bKeepListening);

	bool canEnterRemoteDesktop() const;
	bool canLeaveRemoteDesktop() const;
	bool canStartControlledStreaming() const;
	bool canSendInput() const;
	bool canReceiveInput() const;
	bool canSendVideo() const;
	bool canSyncClipboard() const;
	bool isConnecting() const;
	bool isAwaitingApproval() const;
	bool isNegotiating() const;
	bool isReconnecting() const;
	bool isStopping() const;
	bool hasActiveSession() const;
	bool canHandlePeerTermination() const;
	bool shouldKeepListening() const;

	static bool roleFromString(const QString &strRole, KSessionRole *pRole);
	static QString roleName(KSessionRole role);
	static QString stateName(KSessionState state);
	static QString endReasonName(KSessionEndReason reason, const QString &strDetail);

private:
	KSessionRole m_role = ControllerSessionRole;
	KSessionState m_state = IdleSessionState;
	bool m_bRestoreStreaming = false;
	quint64 m_nGeneration = 0;
};

#endif // _WINREMOTECONTROL_CORE_SESSION_SESSIONSTATEMACHINE_H_
