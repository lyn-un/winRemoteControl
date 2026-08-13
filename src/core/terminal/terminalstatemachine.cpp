#include "core/terminal/terminalstatemachine.h"

KTerminalState KTerminalStateMachine::state() const
{
	return m_state;
}

bool KTerminalStateMachine::canTransitionTo(KTerminalState nextState) const
{
	if (nextState == m_state)
		return true;
	if (nextState == FailedTerminalState)
		return m_state != ClosedTerminalState && m_state != ClosingTerminalState;
	switch (m_state)
	{
	case ClosedTerminalState:
		return nextState == OpeningTerminalState
			|| nextState == AwaitingApprovalTerminalState;
	case AwaitingApprovalTerminalState:
		return nextState == OpeningTerminalState || nextState == ClosingTerminalState
			|| nextState == ClosedTerminalState;
	case OpeningTerminalState:
		return nextState == AwaitingApprovalTerminalState || nextState == RunningTerminalState
			|| nextState == ClosingTerminalState || nextState == ClosedTerminalState;
	case RunningTerminalState:
		return nextState == PausedTerminalState || nextState == ClosingTerminalState
			|| nextState == ClosedTerminalState;
	case PausedTerminalState:
		return nextState == RunningTerminalState || nextState == ClosingTerminalState
			|| nextState == ClosedTerminalState;
	case FailedTerminalState:
		return nextState == ClosingTerminalState || nextState == ClosedTerminalState;
	case ClosingTerminalState:
		return nextState == ClosedTerminalState;
	default:
		return false;
	}
}

bool KTerminalStateMachine::transitionTo(KTerminalState nextState)
{
	if (!canTransitionTo(nextState))
		return false;
	m_state = nextState;
	return true;
}

void KTerminalStateMachine::reset()
{
	m_state = ClosedTerminalState;
}
