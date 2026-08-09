#include "core/session/sessionstatemachine.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

#include <functional>
#include <vector>

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;

		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void testControllerLifecycle()
	{
		KSessionStateMachine stateMachine;
		check(stateMachine.role() == ControllerSessionRole, QStringLiteral("controller is default role"));
		check(stateMachine.state() == IdleSessionState, QStringLiteral("session starts idle"));
		check(!stateMachine.beginStopping(), QStringLiteral("idle session cannot stop again"));
		check(stateMachine.beginConnecting(), QStringLiteral("controller can begin connecting"));
		check(stateMachine.generation() == 1, QStringLiteral("new connection increments generation"));
		check(stateMachine.isConnecting(), QStringLiteral("connecting state is recognized"));
		check(stateMachine.beginAwaitingApproval(), QStringLiteral("connection waits for approval"));
		check(stateMachine.isAwaitingApproval(), QStringLiteral("approval state is recognized"));
		check(stateMachine.approveConnection(), QStringLiteral("approved connection can negotiate"));
		check(stateMachine.markConnected(), QStringLiteral("negotiation can become connected"));
		check(stateMachine.canEnterRemoteDesktop(), QStringLiteral("connected controller can enter desktop"));
		check(stateMachine.beginStreaming(), QStringLiteral("connected controller can stream"));
		check(stateMachine.canSendInput(), QStringLiteral("streaming controller can send input"));
		check(stateMachine.canSyncClipboard(), QStringLiteral("streaming controller can sync clipboard"));
		check(stateMachine.canLeaveRemoteDesktop(), QStringLiteral("streaming controller can leave desktop"));
		check(!stateMachine.markConnected(),
			QStringLiteral("duplicate channel event cannot regress an active stream"));
		check(stateMachine.stopStreaming(), QStringLiteral("controller can stop streaming"));
		check(stateMachine.state() == ConnectedSessionState, QStringLiteral("stop returns to connected"));
		check(stateMachine.beginStopping(), QStringLiteral("controller can stop session"));
		check(!stateMachine.canHandlePeerTermination(),
			QStringLiteral("stopping session ignores peer termination"));
		check(stateMachine.markShutdownTimedOut(),
			QStringLiteral("stopping session can enter shutdown timeout isolation"));
		check(stateMachine.isShutdownTimedOut()
			&& stateMachine.canCompleteShutdown()
			&& !stateMachine.canSendInput(),
			QStringLiteral("shutdown timeout remains completable but disables input"));
		check(!stateMachine.beginStopping(),
			QStringLiteral("timed-out shutdown cannot be started again"));
		stateMachine.finish(false);
		check(stateMachine.state() == IdleSessionState, QStringLiteral("controller finishes idle"));
		check(!stateMachine.beginStopping(), QStringLiteral("finished controller cannot stop twice"));
		check(stateMachine.beginConnecting(), QStringLiteral("controller can reconnect"));
		check(stateMachine.generation() == 2, QStringLiteral("reconnect gets a new generation"));
	}

	void testControlledLifecycleAndGeneration()
	{
		KSessionStateMachine stateMachine;
		check(stateMachine.beginListening(), QStringLiteral("controlled host can listen"));
		check(stateMachine.role() == ControlledSessionRole, QStringLiteral("listening selects controlled role"));
		check(stateMachine.generation() == 1, QStringLiteral("listener generation starts"));
		check(stateMachine.beginAwaitingApproval(), QStringLiteral("incoming connection awaits approval"));
		check(stateMachine.generation() == 2, QStringLiteral("incoming session increments generation"));
		check(stateMachine.approveConnection(), QStringLiteral("incoming connection is approved"));
		check(stateMachine.markConnected(), QStringLiteral("controlled host connects"));
		check(stateMachine.canStartControlledStreaming(),
			QStringLiteral("connected controlled host can start streaming"));
		check(stateMachine.beginStreaming(), QStringLiteral("controlled host begins streaming"));
		check(stateMachine.canReceiveInput() && stateMachine.canSendVideo(),
			QStringLiteral("streaming controlled host enables media and input"));
		check(stateMachine.canSyncClipboard(),
			QStringLiteral("streaming controlled host can sync clipboard"));
		check(stateMachine.beginStopping(), QStringLiteral("controlled session starts stopping"));
		check(!stateMachine.beginStopping(), QStringLiteral("duplicate stop is rejected"));
		stateMachine.finish(true);
		check(stateMachine.state() == ListeningSessionState,
			QStringLiteral("controlled host returns to listening"));
		check(stateMachine.beginAwaitingApproval(), QStringLiteral("next incoming session awaits approval"));
		check(stateMachine.generation() == 3, QStringLiteral("next session gets a new generation"));
		check(stateMachine.rejectConnection(), QStringLiteral("pending connection can be rejected"));
		check(stateMachine.state() == ListeningSessionState,
			QStringLiteral("rejected incoming connection restores listening"));
	}

	void testInitializationRollbackTimeout()
	{
		KSessionStateMachine stateMachine;
		check(stateMachine.markInitializationRollbackTimedOut(),
			QStringLiteral("idle initialization rollback can enter timeout isolation"));
		check(stateMachine.isShutdownTimedOut()
			&& !stateMachine.canSendInput(),
			QStringLiteral("initialization rollback timeout disables session work"));
		stateMachine.finish(false);
		check(stateMachine.state() == IdleSessionState,
			QStringLiteral("late initialization rollback completion restores idle"));
	}

	void testInterruptionRestore()
	{
		KSessionStateMachine stateMachine;
		stateMachine.beginConnecting();
		stateMachine.beginAwaitingApproval();
		stateMachine.approveConnection();
		stateMachine.markConnected();
		stateMachine.beginStreaming();
		check(stateMachine.beginReconnecting(), QStringLiteral("active stream can begin reconnecting"));
		check(stateMachine.isReconnecting(), QStringLiteral("reconnecting state is recognized"));
		check(!stateMachine.canSendInput(), QStringLiteral("input is disabled while reconnecting"));
		check(!stateMachine.canSendVideo(), QStringLiteral("video sending is disabled while reconnecting"));
		check(!stateMachine.canSyncClipboard(),
			QStringLiteral("clipboard sync is disabled while reconnecting"));
		check(!stateMachine.beginReconnecting(),
			QStringLiteral("duplicate reconnecting transition is rejected"));
		check(stateMachine.restore(), QStringLiteral("interrupted stream can restore"));
		check(stateMachine.state() == StreamingSessionState,
			QStringLiteral("streaming state is restored"));

		stateMachine.stopStreaming();
		check(stateMachine.beginReconnecting(), QStringLiteral("connected session can begin reconnecting"));
		check(stateMachine.restore(), QStringLiteral("connected session can restore"));
		check(stateMachine.state() == ConnectedSessionState,
			QStringLiteral("connected state is restored"));
	}

	void testInvalidTransitionsAndRoleChanges()
	{
		KSessionStateMachine stateMachine;
		check(!stateMachine.beginStreaming(), QStringLiteral("idle session cannot stream"));
		check(!stateMachine.markConnected(), QStringLiteral("idle session cannot become connected"));
		check(stateMachine.setRole(ControlledSessionRole), QStringLiteral("idle role can change"));
		check(stateMachine.beginListening(), QStringLiteral("selected controlled role can listen"));
		check(!stateMachine.setRole(ControllerSessionRole), QStringLiteral("active role cannot change"));
		check(!stateMachine.beginReconnecting(), QStringLiteral("listener cannot begin reconnecting"));
		check(!stateMachine.canHandlePeerTermination(),
			QStringLiteral("listener ignores peer termination"));
		check(stateMachine.beginStopping(), QStringLiteral("listener can be stopped explicitly"));
		stateMachine.finish(false);
		check(!stateMachine.beginStopping(), QStringLiteral("stopped listener cannot stop twice"));
	}

	void testEndReasonNames()
	{
		struct KEndReasonCase
		{
			KSessionEndReason reason;
			const char *pExpectedName;
		};
		const KEndReasonCase cases[] = {
			{ LocalDisconnectSessionEndReason, "local_disconnect" },
			{ RoleChangedSessionEndReason, "role_changed" },
			{ RestartListenerSessionEndReason, "restart_listener" },
			{ NewConnectionSessionEndReason, "new_connection" },
			{ ControlledUserStopSessionEndReason, "controlled_user_stop" },
			{ CaptureFailedSessionEndReason, "capture_failed" },
			{ InputChannelClosedSessionEndReason, "input_channel_closed" },
			{ SessionChannelClosedSessionEndReason, "session_channel_closed" },
			{ ConnectFailedSessionEndReason, "connect_failed" },
			{ SignalingLostSessionEndReason, "signaling_lost_during_negotiation" },
			{ DisconnectTimeoutSessionEndReason, "ice_disconnected_timeout" }
		};
		for (const KEndReasonCase &endReasonCase : cases)
		{
			check(KSessionStateMachine::endReasonName(endReasonCase.reason, QString())
				== QString::fromLatin1(endReasonCase.pExpectedName),
				QStringLiteral("session end reason keeps its wire/log name"));
		}
		check(KSessionStateMachine::endReasonName(PeerTerminatedSessionEndReason,
			QStringLiteral("ice_failed")) == QStringLiteral("ice_failed"),
			QStringLiteral("peer termination keeps transport detail"));
		check(KSessionStateMachine::endReasonName(RemoteStopSessionEndReason,
			QStringLiteral("controlled_user_stop")) == QStringLiteral("remote_controlled_user_stop"),
			QStringLiteral("remote reason keeps compatibility prefix"));

		KSessionRole role;
		check(KSessionStateMachine::roleFromString(QStringLiteral("controller"), &role)
			&& role == ControllerSessionRole,
			QStringLiteral("controller role string decodes"));
		check(!KSessionStateMachine::roleFromString(QStringLiteral("unknown"), &role),
			QStringLiteral("unknown role string is rejected"));
	}

	void testTableDrivenTransitions()
	{
		struct KTransitionCase
		{
			const char *pDescription = nullptr;
			std::function<void(KSessionStateMachine &)> prepare;
			std::function<bool(KSessionStateMachine &)> transition;
			bool bExpectedResult = false;
			KSessionState expectedState = IdleSessionState;
		};
		const std::vector<KTransitionCase> cases = {
			{ "idle cannot stop", {},
				[](KSessionStateMachine &machine) { return machine.beginStopping(); },
				false, IdleSessionState },
			{ "idle controller can connect", {},
				[](KSessionStateMachine &machine) { return machine.beginConnecting(); },
				true, ConnectingSessionState },
			{ "listener cannot reconnect",
				[](KSessionStateMachine &machine) { machine.beginListening(); },
				[](KSessionStateMachine &machine) { return machine.beginReconnecting(); },
				false, ListeningSessionState },
			{ "connected session can reconnect",
				[](KSessionStateMachine &machine)
				{
					machine.beginConnecting();
					machine.beginAwaitingApproval();
					machine.approveConnection();
					machine.markConnected();
				},
				[](KSessionStateMachine &machine) { return machine.beginReconnecting(); },
				true, ReconnectingSessionState }
		};

		for (const KTransitionCase &transitionCase : cases)
		{
			KSessionStateMachine stateMachine;
			if (transitionCase.prepare)
				transitionCase.prepare(stateMachine);
			const bool bResult = transitionCase.transition(stateMachine);
			check(bResult == transitionCase.bExpectedResult
				&& stateMachine.state() == transitionCase.expectedState,
				QString::fromLatin1(transitionCase.pDescription));
		}
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testControllerLifecycle();
	testControlledLifecycleAndGeneration();
	testInitializationRollbackTimeout();
	testInterruptionRestore();
	testInvalidTransitionsAndRoleChanges();
	testTableDrivenTransitions();
	testEndReasonNames();

	if (g_nFailureCount == 0)
		qInfo() << "All session state machine tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
