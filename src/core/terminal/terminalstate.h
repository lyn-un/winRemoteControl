#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALSTATE_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALSTATE_H_

#include <QtCore/QString>

enum KTerminalState
{
	ClosedTerminalState,
	AwaitingApprovalTerminalState,
	OpeningTerminalState,
	RunningTerminalState,
	PausedTerminalState,
	ClosingTerminalState,
	FailedTerminalState
};

inline QString TerminalStateName(KTerminalState state)
{
	switch (state)
	{
	case AwaitingApprovalTerminalState: return QStringLiteral("AwaitingApproval");
	case OpeningTerminalState: return QStringLiteral("Opening");
	case RunningTerminalState: return QStringLiteral("Running");
	case PausedTerminalState: return QStringLiteral("Paused");
	case ClosingTerminalState: return QStringLiteral("Closing");
	case FailedTerminalState: return QStringLiteral("Failed");
	case ClosedTerminalState:
	default: return QStringLiteral("Closed");
	}
}

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALSTATE_H_
