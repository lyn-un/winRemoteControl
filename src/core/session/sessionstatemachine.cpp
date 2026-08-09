#include "core/session/sessionstatemachine.h"

KSessionRole KSessionStateMachine::role() const
{
	return m_role;
}

KSessionState KSessionStateMachine::state() const
{
	return m_state;
}

quint64 KSessionStateMachine::generation() const
{
	return m_nGeneration;
}

bool KSessionStateMachine::setRole(KSessionRole role)
{
	if (m_state != IdleSessionState)
		return false;

	m_role = role;
	return true;
}

bool KSessionStateMachine::beginListening()
{
	if (m_state != IdleSessionState)
		return false;

	m_role = ControlledSessionRole;
	m_state = ListeningSessionState;
	m_bRestoreStreaming = false;
	++m_nGeneration;
	return true;
}

bool KSessionStateMachine::beginConnecting()
{
	if (m_state != IdleSessionState)
		return false;

	m_role = ControllerSessionRole;
	m_state = ConnectingSessionState;
	m_bRestoreStreaming = false;
	++m_nGeneration;
	return true;
}

bool KSessionStateMachine::beginAwaitingApproval()
{
	if (m_state == ListeningSessionState)
	{
		++m_nGeneration;
		m_state = AwaitingApprovalSessionState;
		return true;
	}
	if (m_state != ConnectingSessionState)
		return false;

	m_state = AwaitingApprovalSessionState;
	return true;
}

bool KSessionStateMachine::approveConnection()
{
	if (m_state != AwaitingApprovalSessionState)
		return false;

	m_state = NegotiatingSessionState;
	return true;
}

bool KSessionStateMachine::rejectConnection()
{
	if (m_state != AwaitingApprovalSessionState)
		return false;

	m_state = m_role == ControlledSessionRole ? ListeningSessionState : IdleSessionState;
	m_bRestoreStreaming = false;
	return true;
}

bool KSessionStateMachine::markConnected()
{
	if (m_state != NegotiatingSessionState && m_state != ConnectedSessionState)
		return false;

	m_state = ConnectedSessionState;
	m_bRestoreStreaming = false;
	return true;
}

bool KSessionStateMachine::beginStreaming()
{
	if (m_state != ConnectedSessionState)
		return false;

	m_state = StreamingSessionState;
	m_bRestoreStreaming = false;
	return true;
}

bool KSessionStateMachine::stopStreaming()
{
	if (m_state != StreamingSessionState)
		return false;

	m_state = ConnectedSessionState;
	m_bRestoreStreaming = false;
	return true;
}

bool KSessionStateMachine::beginReconnecting()
{
	if (!canHandlePeerTermination() || m_state == ReconnectingSessionState)
		return false;

	m_bRestoreStreaming = m_state == StreamingSessionState;
	m_state = ReconnectingSessionState;
	return true;
}

bool KSessionStateMachine::restore()
{
	if (m_state != ReconnectingSessionState)
		return false;

	m_state = m_bRestoreStreaming ? StreamingSessionState : ConnectedSessionState;
	m_bRestoreStreaming = false;
	return true;
}

bool KSessionStateMachine::beginStopping()
{
	if (m_state == IdleSessionState
		|| m_state == StoppingSessionState
		|| m_state == ShutdownTimedOutSessionState)
		return false;

	m_state = StoppingSessionState;
	return true;
}

bool KSessionStateMachine::markShutdownTimedOut()
{
	if (m_state != StoppingSessionState)
		return false;
	m_state = ShutdownTimedOutSessionState;
	return true;
}

void KSessionStateMachine::finish(bool bKeepListening)
{
	m_state = bKeepListening && m_role == ControlledSessionRole
		? ListeningSessionState
		: IdleSessionState;
	m_bRestoreStreaming = false;
}

bool KSessionStateMachine::canEnterRemoteDesktop() const
{
	return m_role == ControllerSessionRole && m_state == ConnectedSessionState;
}

bool KSessionStateMachine::canLeaveRemoteDesktop() const
{
	return m_role == ControllerSessionRole && m_state == StreamingSessionState;
}

bool KSessionStateMachine::canStartControlledStreaming() const
{
	return m_role == ControlledSessionRole && m_state == ConnectedSessionState;
}

bool KSessionStateMachine::canSendInput() const
{
	return m_role == ControllerSessionRole && m_state == StreamingSessionState;
}

bool KSessionStateMachine::canReceiveInput() const
{
	return m_role == ControlledSessionRole && m_state == StreamingSessionState;
}

bool KSessionStateMachine::canSendVideo() const
{
	return m_role == ControlledSessionRole && m_state == StreamingSessionState;
}

bool KSessionStateMachine::canSyncClipboard() const
{
	return m_state == StreamingSessionState;
}

bool KSessionStateMachine::isConnecting() const
{
	return m_role == ControllerSessionRole && m_state == ConnectingSessionState;
}

bool KSessionStateMachine::isAwaitingApproval() const
{
	return m_state == AwaitingApprovalSessionState;
}

bool KSessionStateMachine::isNegotiating() const
{
	return m_state == ConnectingSessionState
		|| m_state == AwaitingApprovalSessionState
		|| m_state == NegotiatingSessionState;
}

bool KSessionStateMachine::isReconnecting() const
{
	return m_state == ReconnectingSessionState;
}

bool KSessionStateMachine::isStopping() const
{
	return m_state == StoppingSessionState;
}

bool KSessionStateMachine::isShutdownTimedOut() const
{
	return m_state == ShutdownTimedOutSessionState;
}

bool KSessionStateMachine::canCompleteShutdown() const
{
	return isStopping() || isShutdownTimedOut();
}

bool KSessionStateMachine::hasActiveSession() const
{
	return m_state != IdleSessionState
		&& m_state != ListeningSessionState
		&& m_state != StoppingSessionState
		&& m_state != ShutdownTimedOutSessionState;
}

bool KSessionStateMachine::canHandlePeerTermination() const
{
	return hasActiveSession();
}

bool KSessionStateMachine::shouldKeepListening() const
{
	return m_role == ControlledSessionRole;
}

bool KSessionStateMachine::roleFromString(const QString &strRole, KSessionRole *pRole)
{
	if (pRole == nullptr)
		return false;

	if (strRole == QStringLiteral("controller"))
		*pRole = ControllerSessionRole;
	else if (strRole == QStringLiteral("controlled"))
		*pRole = ControlledSessionRole;
	else
		return false;
	return true;
}

QString KSessionStateMachine::roleName(KSessionRole role)
{
	return role == ControlledSessionRole
		? QStringLiteral("controlled")
		: QStringLiteral("controller");
}

QString KSessionStateMachine::stateName(KSessionState state)
{
	if (state == IdleSessionState)
		return QStringLiteral("Idle");
	if (state == ListeningSessionState)
		return QStringLiteral("Listening");
	if (state == ConnectingSessionState)
		return QStringLiteral("Connecting");
	if (state == AwaitingApprovalSessionState)
		return QStringLiteral("AwaitingApproval");
	if (state == NegotiatingSessionState)
		return QStringLiteral("Negotiating");
	if (state == ConnectedSessionState)
		return QStringLiteral("Connected");
	if (state == StreamingSessionState)
		return QStringLiteral("Streaming");
	if (state == ReconnectingSessionState)
		return QStringLiteral("Reconnecting");
	if (state == StoppingSessionState)
		return QStringLiteral("Stopping");
	return QStringLiteral("ShutdownTimedOut");
}

QString KSessionStateMachine::endReasonName(KSessionEndReason reason, const QString &strDetail)
{
	if (reason == LocalDisconnectSessionEndReason)
		return QStringLiteral("local_disconnect");
	if (reason == RoleChangedSessionEndReason)
		return QStringLiteral("role_changed");
	if (reason == RestartListenerSessionEndReason)
		return QStringLiteral("restart_listener");
	if (reason == NewConnectionSessionEndReason)
		return QStringLiteral("new_connection");
	if (reason == ControlledUserStopSessionEndReason)
		return QStringLiteral("controlled_user_stop");
	if (reason == CaptureFailedSessionEndReason)
		return QStringLiteral("capture_failed");
	if (reason == InputChannelClosedSessionEndReason)
		return QStringLiteral("input_channel_closed");
	if (reason == SessionChannelClosedSessionEndReason)
		return QStringLiteral("session_channel_closed");
	if (reason == ConnectFailedSessionEndReason)
		return QStringLiteral("connect_failed");
	if (reason == SignalingLostSessionEndReason)
		return QStringLiteral("signaling_lost_during_negotiation");
	if (reason == DisconnectTimeoutSessionEndReason)
		return QStringLiteral("ice_disconnected_timeout");
	if (reason == PeerTerminatedSessionEndReason)
		return strDetail.isEmpty() ? QStringLiteral("peer_terminated") : strDetail;
	if (strDetail.isEmpty())
		return QStringLiteral("remote_stop");
	return QStringLiteral("remote_%1").arg(strDetail);
}
