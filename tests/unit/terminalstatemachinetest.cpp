#include "core/terminal/terminalstatemachine.h"

#include <QtCore/QCoreApplication>

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	KTerminalStateMachine machine;
	if (!machine.transitionTo(AwaitingApprovalTerminalState)
		|| machine.transitionTo(RunningTerminalState)
		|| !machine.transitionTo(OpeningTerminalState)
		|| !machine.transitionTo(RunningTerminalState)
		|| !machine.transitionTo(PausedTerminalState)
		|| !machine.transitionTo(RunningTerminalState)
		|| !machine.transitionTo(ClosingTerminalState)
		|| !machine.transitionTo(ClosedTerminalState))
	{
		return 1;
	}
	return 0;
}
