#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALSTATEMACHINE_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALSTATEMACHINE_H_

#include "core/terminal/terminalstate.h"

class KTerminalStateMachine
{
public:
	KTerminalState state() const;
	bool canTransitionTo(KTerminalState nextState) const;
	bool transitionTo(KTerminalState nextState);
	void reset();

private:
	KTerminalState m_state = ClosedTerminalState;
};

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALSTATEMACHINE_H_
