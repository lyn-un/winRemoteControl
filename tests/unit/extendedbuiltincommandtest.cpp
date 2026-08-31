#include "commands/applicationcommandregistry.h"
#include "commands/extendedbuiltincommands.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	KExtendedApplicationCommandCallbacks CreateCallbacks(
		QByteArray *pTerminalInput,
		QStringList *pCopiedEntryIds,
		int *pWindowCallCount)
	{
		KExtendedApplicationCommandCallbacks callbacks;
		callbacks.startLocalPreview = []() {};
		callbacks.stopLocalPreview = []() {};
		callbacks.refreshLanDevices = []() {};
		callbacks.connectLanDevice = [](const QString &) {};
		callbacks.requestRecentDevices = []() {};
		callbacks.connectRecentDevice = [](const QString &) {};
		callbacks.removeRecentDevice = [](const QString &) {};
		callbacks.openRecentDeviceTerminal = [](const QString &) {};
		callbacks.retryLastConnection = []() {};
		callbacks.applicationSettings = []() { return KApplicationSettings(); };
		callbacks.updateApplicationSettings = [](bool, const QString &, int, int) {};
		callbacks.updateApplicationTheme = [](const QString &) {};
		callbacks.requestTrustedDevices = []() {};
		callbacks.updateTrustedDevice = [](const QString &, const QString &,
			KPermissionScopes) {};
		callbacks.revokeTrustedDevice = [](const QString &) {};
		callbacks.requestRePairDevice = [](const QString &) {};
		callbacks.setClipboardSyncEnabled = [](bool) {};
		callbacks.requestClipboardSyncState = []() {};
		callbacks.openCurrentTerminal = [](int, int) {};
		callbacks.respondTerminalRequest = [](const QString &, bool) {};
		callbacks.sendTerminalInput = [pTerminalInput](const QByteArray &data)
		{ *pTerminalInput = data; };
		callbacks.resizeTerminal = [](int, int) {};
		callbacks.closeTerminal = []() {};
		callbacks.requestTerminalState = []() {};
		callbacks.isFileTransferAvailable = []() { return true; };
		callbacks.openFileTransfer = []() {};
		callbacks.stopFileTransfer = []() {};
		callbacks.requestFileTransferSnapshot = []() {};
		callbacks.requestFileTransferPaneRoots = [](KFileTransferPane) {};
		callbacks.navigateFileTransferPane = [](KFileTransferPane,
			const QString &, const QString &) {};
		callbacks.navigateFileTransferPaneByPath = [](KFileTransferPane,
			const QString &) {};
		callbacks.navigateFileTransferPaneUp = [](KFileTransferPane,
			const QString &) {};
		callbacks.refreshFileTransferPane = [](KFileTransferPane) {};
		callbacks.startFileCopy = [pCopiedEntryIds](KFileTransferPane,
			const QString &, const QStringList &entryIdList, const QString &)
		{ *pCopiedEntryIds = entryIdList; };
		callbacks.pauseFileTransferTask = [](const QString &) {};
		callbacks.resumeFileTransferTask = [](const QString &) {};
		callbacks.cancelFileTransferTask = [](const QString &) {};
		callbacks.retryFileTransferTask = [](const QString &) {};
		callbacks.resolveFileTransferConflict = [](const QString &,
			KFileTransferConflictResolution, bool) {};
		callbacks.clearCompletedFileTransferTasks = []() {};
		auto windowAction = [pWindowCallCount]()
		{
			++(*pWindowCallCount);
			return true;
		};
		callbacks.minimizeMainWindow = windowAction;
		callbacks.closeMainWindow = windowAction;
		callbacks.minimizeDesktopWindow = windowAction;
		callbacks.toggleMaximizeDesktopWindow = windowAction;
		callbacks.closeDesktopWindow = windowAction;
		callbacks.minimizeFileTransferWindow = windowAction;
		callbacks.toggleMaximizeFileTransferWindow = windowAction;
		callbacks.closeFileTransferWindow = windowAction;
		return callbacks;
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QByteArray terminalInput;
	QStringList copiedEntryIds;
	int nWindowCallCount = 0;
	KApplicationCommandRegistry registry;
	const KExtendedApplicationCommandCallbacks callbacks = CreateCallbacks(
		&terminalInput, &copiedEntryIds, &nWindowCallCount);
	Check(RegisterExtendedBuiltinApplicationCommands(&registry, callbacks),
		QStringLiteral("extended commands register"));
	for (const QString &strCommandId : {
		QStringLiteral("capture.preview.start"),
		QStringLiteral("discovery.refresh"),
		QStringLiteral("settings.update"),
		QStringLiteral("trusted.revoke"),
		QStringLiteral("clipboard.set_enabled"),
		QStringLiteral("terminal.input"),
		QStringLiteral("file_transfer.copy"),
		QStringLiteral("window.main.minimize")})
	{
		Check(registry.contains(strCommandId),
			QStringLiteral("registered %1").arg(strCommandId));
	}

	const KApplicationCommandResult terminalResult = registry.execute(
		QStringLiteral("terminal.input"),
		QJsonObject{{QStringLiteral("dataBase64"), QStringLiteral("YWJj")}});
	Check(terminalResult.status == ApplicationCommandSucceeded
		&& terminalInput == QByteArrayLiteral("abc"),
		QStringLiteral("terminal input validates and decodes base64"));

	const KApplicationCommandResult copyResult = registry.execute(
		QStringLiteral("file_transfer.copy"), QJsonObject{
			{QStringLiteral("pane"), QStringLiteral("local")},
			{QStringLiteral("sourceListingId"), QStringLiteral("source")},
			{QStringLiteral("destinationListingId"), QStringLiteral("destination")},
			{QStringLiteral("entryIds"), QJsonArray{
				QStringLiteral("entry-1"), QStringLiteral("entry-2")}}
		});
	Check(copyResult.status == ApplicationCommandSucceeded
		&& copiedEntryIds == QStringList{
			QStringLiteral("entry-1"), QStringLiteral("entry-2")},
		QStringLiteral("file copy only forwards opaque ids"));

	const KApplicationCommandResult invalidSettings = registry.execute(
		QStringLiteral("settings.update"),
		QJsonObject{{QStringLiteral("approvalTimeoutSeconds"), 30.5}});
	Check(invalidSettings.status == ApplicationCommandInvalidArgument,
		QStringLiteral("fractional settings integers are rejected"));

	const KApplicationCommandResult windowResult = registry.execute(
		QStringLiteral("window.main.minimize"), QJsonObject());
	Check(windowResult.status == ApplicationCommandSucceeded && nWindowCallCount == 1,
		QStringLiteral("window action executes exactly once"));

	if (g_nFailureCount == 0)
		qInfo() << "All extended application command tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
