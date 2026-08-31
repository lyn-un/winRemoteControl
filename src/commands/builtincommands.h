#ifndef _WINREMOTECONTROL_BUILTINCOMMANDS_H_
#define _WINREMOTECONTROL_BUILTINCOMMANDS_H_

#include <QtCore/QString>

#include <functional>

class KApplicationCommandRegistry;
class KSessionController;
class KSessionViewModel;
class KDeviceSecurityPreferenceService;

bool RegisterBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	KSessionViewModel *pSessionViewModel,
	KSessionController *pSessionController,
	KDeviceSecurityPreferenceService *pSecurityPreferenceService,
	const std::function<void(const QString &)> &roleChanged = {});

#endif // _WINREMOTECONTROL_BUILTINCOMMANDS_H_
