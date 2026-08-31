#ifndef _WINREMOTECONTROL_EXTENDEDBUILTINCOMMANDS_H_
#define _WINREMOTECONTROL_EXTENDEDBUILTINCOMMANDS_H_

#include "core/file_transfer/filetransferstate.h"
#include "core/security/permissionscope.h"
#include "core/settings/applicationsettings.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

class KApplicationCommandRegistry;

struct KExtendedApplicationCommandCallbacks
{
	std::function<void()> startLocalPreview;
	std::function<void()> stopLocalPreview;
	std::function<void()> refreshLanDevices;
	std::function<void(const QString &)> connectLanDevice;
	std::function<void()> requestRecentDevices;
	std::function<void(const QString &)> connectRecentDevice;
	std::function<void(const QString &)> removeRecentDevice;
	std::function<void(const QString &)> openRecentDeviceTerminal;
	std::function<void()> retryLastConnection;

	std::function<KApplicationSettings()> applicationSettings;
	std::function<void(bool, const QString &, int, int)> updateApplicationSettings;
	std::function<void(const QString &)> updateApplicationTheme;
	std::function<void()> requestTrustedDevices;
	std::function<void(const QString &, const QString &, KPermissionScopes)>
		updateTrustedDevice;
	std::function<void(const QString &)> revokeTrustedDevice;
	std::function<void(const QString &)> requestRePairDevice;

	std::function<void(bool)> setClipboardSyncEnabled;
	std::function<void()> requestClipboardSyncState;
	std::function<void(int, int)> openCurrentTerminal;
	std::function<void(const QString &, bool)> respondTerminalRequest;
	std::function<void(const QByteArray &)> sendTerminalInput;
	std::function<void(int, int)> resizeTerminal;
	std::function<void()> closeTerminal;
	std::function<void()> requestTerminalState;

	std::function<bool()> isFileTransferAvailable;
	std::function<void()> openFileTransfer;
	std::function<void()> stopFileTransfer;
	std::function<void()> requestFileTransferSnapshot;
	std::function<void(KFileTransferPane)> requestFileTransferPaneRoots;
	std::function<void(KFileTransferPane, const QString &, const QString &)>
		navigateFileTransferPane;
	std::function<void(KFileTransferPane, const QString &)> navigateFileTransferPaneByPath;
	std::function<void(KFileTransferPane, const QString &)> navigateFileTransferPaneUp;
	std::function<void(KFileTransferPane)> refreshFileTransferPane;
	std::function<void(KFileTransferPane, const QString &, const QStringList &, const QString &)>
		startFileCopy;
	std::function<void(const QString &)> pauseFileTransferTask;
	std::function<void(const QString &)> resumeFileTransferTask;
	std::function<void(const QString &)> cancelFileTransferTask;
	std::function<void(const QString &)> retryFileTransferTask;
	std::function<void(const QString &, KFileTransferConflictResolution, bool)>
		resolveFileTransferConflict;
	std::function<void()> clearCompletedFileTransferTasks;

	std::function<bool()> minimizeMainWindow;
	std::function<bool()> closeMainWindow;
	std::function<bool()> minimizeDesktopWindow;
	std::function<bool()> toggleMaximizeDesktopWindow;
	std::function<bool()> closeDesktopWindow;
	std::function<bool()> minimizeFileTransferWindow;
	std::function<bool()> toggleMaximizeFileTransferWindow;
	std::function<bool()> closeFileTransferWindow;
};

bool RegisterExtendedBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks);

#endif // _WINREMOTECONTROL_EXTENDEDBUILTINCOMMANDS_H_
