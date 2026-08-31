#include "commands/extendedbuiltincommands.h"

#include "commands/applicationcommandregistry.h"

namespace
{
	bool RegisterWindowCommand(KApplicationCommandRegistry *pRegistry,
		const QString &strId,
		const std::function<bool()> &callback)
	{
		KApplicationCommand command;
		command.strId = strId;
		command.execute = [callback](const QJsonObject &)
		{
			KApplicationCommandResult result;
			if (!callback())
			{
				result.status = ApplicationCommandDisabled;
				result.strErrorCode = QStringLiteral("command_disabled");
				result.strTechnicalMessage = QStringLiteral("Window is not available");
				return result;
			}
			result.status = ApplicationCommandSucceeded;
			return result;
		};
		return pRegistry->registerCommand(command, nullptr);
	}
}

bool RegisterWindowBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks)
{
	if (pRegistry == nullptr || !callbacks.minimizeMainWindow || !callbacks.closeMainWindow
		|| !callbacks.minimizeDesktopWindow || !callbacks.toggleMaximizeDesktopWindow
		|| !callbacks.closeDesktopWindow || !callbacks.minimizeFileTransferWindow
		|| !callbacks.toggleMaximizeFileTransferWindow
		|| !callbacks.closeFileTransferWindow)
	{
		return false;
	}

	return RegisterWindowCommand(pRegistry, QStringLiteral("window.main.minimize"),
		callbacks.minimizeMainWindow)
		&& RegisterWindowCommand(pRegistry, QStringLiteral("window.main.close"),
			callbacks.closeMainWindow)
		&& RegisterWindowCommand(pRegistry, QStringLiteral("window.desktop.minimize"),
			callbacks.minimizeDesktopWindow)
		&& RegisterWindowCommand(pRegistry, QStringLiteral("window.desktop.toggle_maximize"),
			callbacks.toggleMaximizeDesktopWindow)
		&& RegisterWindowCommand(pRegistry, QStringLiteral("window.desktop.close"),
			callbacks.closeDesktopWindow)
		&& RegisterWindowCommand(pRegistry, QStringLiteral("window.file_transfer.minimize"),
			callbacks.minimizeFileTransferWindow)
		&& RegisterWindowCommand(pRegistry,
			QStringLiteral("window.file_transfer.toggle_maximize"),
			callbacks.toggleMaximizeFileTransferWindow)
		&& RegisterWindowCommand(pRegistry, QStringLiteral("window.file_transfer.close"),
			callbacks.closeFileTransferWindow);
}
