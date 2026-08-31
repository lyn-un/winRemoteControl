#include "commands/extendedbuiltincommands.h"

class KApplicationCommandRegistry;

bool RegisterDeviceBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks);
bool RegisterSessionFeatureBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks);
bool RegisterWindowBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks);

bool RegisterExtendedBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks)
{
	return RegisterDeviceBuiltinApplicationCommands(pRegistry, callbacks)
		&& RegisterSessionFeatureBuiltinApplicationCommands(pRegistry, callbacks)
		&& RegisterWindowBuiltinApplicationCommands(pRegistry, callbacks);
}
