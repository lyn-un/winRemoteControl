#include "commands/extendedbuiltincommands.h"

#include "commands/applicationcommandregistry.h"
#include "core/protocol/terminaldataframe.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace
{
	KApplicationCommandResult SucceededResult()
	{
		KApplicationCommandResult result;
		result.status = ApplicationCommandSucceeded;
		return result;
	}

	KApplicationCommandResult InvalidArgumentResult(const QString &strMessage)
	{
		KApplicationCommandResult result;
		result.status = ApplicationCommandInvalidArgument;
		result.strErrorCode = QStringLiteral("invalid_argument");
		result.strTechnicalMessage = strMessage;
		return result;
	}

	bool ReadRequiredString(const QJsonObject &arguments,
		const QString &strName,
		QString *pValue)
	{
		if (pValue == nullptr || !arguments.value(strName).isString())
			return false;
		*pValue = arguments.value(strName).toString().trimmed();
		return !pValue->isEmpty();
	}

	bool ReadOptionalInteger(const QJsonObject &arguments,
		const QString &strName,
		int nFallback,
		int *pValue)
	{
		if (pValue == nullptr)
			return false;
		const QJsonValue value = arguments.value(strName);
		if (value.isUndefined())
		{
			*pValue = nFallback;
			return true;
		}
		if (!value.isDouble())
			return false;
		const int nInteger = value.toInt();
		if (value.toDouble() != static_cast<double>(nInteger))
			return false;
		*pValue = nInteger;
		return true;
	}

	bool ReadPane(const QJsonObject &arguments, KFileTransferPane *pPane)
	{
		if (pPane == nullptr)
			return false;
		const QString strPane = arguments.value(QStringLiteral("pane")).toString();
		if (strPane == QStringLiteral("local"))
		{
			*pPane = LocalFileTransferPane;
			return true;
		}
		if (strPane == QStringLiteral("remote"))
		{
			*pPane = RemoteFileTransferPane;
			return true;
		}
		return false;
	}

	bool ReadStringList(const QJsonValue &value, QStringList *pValues)
	{
		if (pValues == nullptr || !value.isArray())
			return false;
		pValues->clear();
		for (const QJsonValue &item : value.toArray())
		{
			if (!item.isString() || item.toString().isEmpty())
				return false;
			pValues->append(item.toString());
		}
		return !pValues->isEmpty();
	}

	bool RegisterCommand(KApplicationCommandRegistry *pRegistry,
		const QString &strId,
		const std::function<KApplicationCommandResult(const QJsonObject &)> &execute,
		const std::function<bool()> &canExecute = std::function<bool()>())
	{
		KApplicationCommand command;
		command.strId = strId;
		command.canExecute = canExecute;
		command.execute = execute;
		return pRegistry->registerCommand(command, nullptr);
	}

	KFileTransferConflictResolution ConflictResolutionFromName(const QString &strName)
	{
		if (strName == QStringLiteral("overwrite"))
			return OverwriteFileTransferConflictResolution;
		if (strName == QStringLiteral("keepBoth"))
			return KeepBothFileTransferConflictResolution;
		if (strName == QStringLiteral("skip"))
			return SkipFileTransferConflictResolution;
		return InvalidFileTransferConflictResolution;
	}
}

bool RegisterSessionFeatureBuiltinApplicationCommands(KApplicationCommandRegistry *pRegistry,
	const KExtendedApplicationCommandCallbacks &callbacks)
{
	if (pRegistry == nullptr || !callbacks.setClipboardSyncEnabled
		|| !callbacks.requestClipboardSyncState || !callbacks.openCurrentTerminal
		|| !callbacks.respondTerminalRequest || !callbacks.sendTerminalInput
		|| !callbacks.resizeTerminal || !callbacks.closeTerminal
		|| !callbacks.requestTerminalState || !callbacks.isFileTransferAvailable
		|| !callbacks.openFileTransfer || !callbacks.stopFileTransfer
		|| !callbacks.requestFileTransferSnapshot || !callbacks.requestFileTransferPaneRoots
		|| !callbacks.navigateFileTransferPane || !callbacks.navigateFileTransferPaneByPath
		|| !callbacks.navigateFileTransferPaneUp || !callbacks.refreshFileTransferPane
		|| !callbacks.startFileCopy || !callbacks.pauseFileTransferTask
		|| !callbacks.resumeFileTransferTask || !callbacks.cancelFileTransferTask
		|| !callbacks.retryFileTransferTask || !callbacks.resolveFileTransferConflict
		|| !callbacks.clearCompletedFileTransferTasks)
	{
		return false;
	}

	if (!RegisterCommand(pRegistry, QStringLiteral("clipboard.set_enabled"),
		[callbacks](const QJsonObject &arguments)
		{
			const QJsonValue enabled = arguments.value(QStringLiteral("enabled"));
			if (!enabled.isBool())
				return InvalidArgumentResult(QStringLiteral("enabled must be a boolean"));
			callbacks.setClipboardSyncEnabled(enabled.toBool());
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("clipboard.get"),
		[callbacks](const QJsonObject &)
		{
			callbacks.requestClipboardSyncState();
			return SucceededResult();
		}))
	{
		return false;
	}

	if (!RegisterCommand(pRegistry, QStringLiteral("terminal.open"),
		[callbacks](const QJsonObject &arguments)
		{
			int nColumns = 0;
			int nRows = 0;
			if (!ReadOptionalInteger(arguments, QStringLiteral("columns"), 100, &nColumns)
				|| !ReadOptionalInteger(arguments, QStringLiteral("rows"), 30, &nRows)
				|| nColumns < 20 || nColumns > 500 || nRows < 5 || nRows > 200)
				return InvalidArgumentResult(QStringLiteral("Terminal dimensions are invalid"));
			callbacks.openCurrentTerminal(nColumns, nRows);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("terminal.respond"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strRequestId;
			const QJsonValue accepted = arguments.value(QStringLiteral("accepted"));
			if (!ReadRequiredString(arguments, QStringLiteral("requestId"), &strRequestId)
				|| !accepted.isBool())
			{
				return InvalidArgumentResult(
					QStringLiteral("requestId and accepted are required"));
			}
			callbacks.respondTerminalRequest(strRequestId, accepted.toBool());
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("terminal.input"),
		[callbacks](const QJsonObject &arguments)
		{
			const QJsonValue dataBase64 = arguments.value(QStringLiteral("dataBase64"));
			if (!dataBase64.isString() || dataBase64.toString().isEmpty())
				return InvalidArgumentResult(QStringLiteral("dataBase64 is required"));
			const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
				dataBase64.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
			if (!decoded || decoded.decoded.isEmpty()
				|| decoded.decoded.size() > KTerminalDataFrameCodec::kMaximumPayloadBytes)
			{
				return InvalidArgumentResult(QStringLiteral("Terminal input is invalid or too large"));
			}
			callbacks.sendTerminalInput(decoded.decoded);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("terminal.resize"),
		[callbacks](const QJsonObject &arguments)
		{
			int nColumns = 0;
			int nRows = 0;
			if (!ReadOptionalInteger(arguments, QStringLiteral("columns"), -1, &nColumns)
				|| !ReadOptionalInteger(arguments, QStringLiteral("rows"), -1, &nRows)
				|| nColumns < 20 || nColumns > 500 || nRows < 5 || nRows > 200)
				return InvalidArgumentResult(QStringLiteral("Terminal dimensions are invalid"));
			callbacks.resizeTerminal(nColumns, nRows);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("terminal.close"),
		[callbacks](const QJsonObject &)
		{
			callbacks.closeTerminal();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("terminal.get"),
		[callbacks](const QJsonObject &)
		{
			callbacks.requestTerminalState();
			return SucceededResult();
		}))
	{
		return false;
	}

	const std::function<bool()> fileTransferAvailable = callbacks.isFileTransferAvailable;
	if (!RegisterCommand(pRegistry, QStringLiteral("file_transfer.open"),
		[callbacks](const QJsonObject &)
		{
			callbacks.openFileTransfer();
			return SucceededResult();
		}, fileTransferAvailable)
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.stop"),
		[callbacks](const QJsonObject &)
		{
			callbacks.stopFileTransfer();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.get"),
		[callbacks](const QJsonObject &)
		{
			callbacks.requestFileTransferSnapshot();
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.roots"),
		[callbacks](const QJsonObject &arguments)
		{
			KFileTransferPane pane;
			if (!ReadPane(arguments, &pane))
				return InvalidArgumentResult(QStringLiteral("pane must be local or remote"));
			callbacks.requestFileTransferPaneRoots(pane);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.navigate"),
		[callbacks](const QJsonObject &arguments)
		{
			KFileTransferPane pane;
			QString strListingId;
			QString strEntryId;
			if (!ReadPane(arguments, &pane)
				|| !ReadRequiredString(arguments, QStringLiteral("listingId"), &strListingId)
				|| !ReadRequiredString(arguments, QStringLiteral("entryId"), &strEntryId))
			{
				return InvalidArgumentResult(
					QStringLiteral("pane, listingId and entryId are required"));
			}
			callbacks.navigateFileTransferPane(pane, strListingId, strEntryId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.navigate_path"),
		[callbacks](const QJsonObject &arguments)
		{
			KFileTransferPane pane;
			QString strPath;
			if (!ReadPane(arguments, &pane)
				|| !ReadRequiredString(arguments, QStringLiteral("path"), &strPath))
			{
				return InvalidArgumentResult(QStringLiteral("pane and path are required"));
			}
			callbacks.navigateFileTransferPaneByPath(pane, strPath);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.up"),
		[callbacks](const QJsonObject &arguments)
		{
			KFileTransferPane pane;
			QString strListingId;
			if (!ReadPane(arguments, &pane)
				|| !ReadRequiredString(arguments, QStringLiteral("listingId"), &strListingId))
			{
				return InvalidArgumentResult(QStringLiteral("pane and listingId are required"));
			}
			callbacks.navigateFileTransferPaneUp(pane, strListingId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.refresh"),
		[callbacks](const QJsonObject &arguments)
		{
			KFileTransferPane pane;
			if (!ReadPane(arguments, &pane))
				return InvalidArgumentResult(QStringLiteral("pane must be local or remote"));
			callbacks.refreshFileTransferPane(pane);
			return SucceededResult();
		}))
	{
		return false;
	}

	if (!RegisterCommand(pRegistry, QStringLiteral("file_transfer.copy"),
		[callbacks](const QJsonObject &arguments)
		{
			KFileTransferPane pane;
			QString strSourceListingId;
			QString strDestinationListingId;
			QStringList entryIdList;
			if (!ReadPane(arguments, &pane)
				|| !ReadRequiredString(arguments, QStringLiteral("sourceListingId"),
					&strSourceListingId)
				|| !ReadRequiredString(arguments, QStringLiteral("destinationListingId"),
					&strDestinationListingId)
				|| !ReadStringList(arguments.value(QStringLiteral("entryIds")), &entryIdList))
			{
				return InvalidArgumentResult(QStringLiteral(
					"pane, sourceListingId, destinationListingId and entryIds are required"));
			}
			callbacks.startFileCopy(
				pane, strSourceListingId, entryIdList, strDestinationListingId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.pause"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strTaskId;
			if (!ReadRequiredString(arguments, QStringLiteral("taskId"), &strTaskId))
				return InvalidArgumentResult(QStringLiteral("taskId is required"));
			callbacks.pauseFileTransferTask(strTaskId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.resume"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strTaskId;
			if (!ReadRequiredString(arguments, QStringLiteral("taskId"), &strTaskId))
				return InvalidArgumentResult(QStringLiteral("taskId is required"));
			callbacks.resumeFileTransferTask(strTaskId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.cancel"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strTaskId;
			if (!ReadRequiredString(arguments, QStringLiteral("taskId"), &strTaskId))
				return InvalidArgumentResult(QStringLiteral("taskId is required"));
			callbacks.cancelFileTransferTask(strTaskId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.retry"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strTaskId;
			if (!ReadRequiredString(arguments, QStringLiteral("taskId"), &strTaskId))
				return InvalidArgumentResult(QStringLiteral("taskId is required"));
			callbacks.retryFileTransferTask(strTaskId);
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.resolve_conflict"),
		[callbacks](const QJsonObject &arguments)
		{
			QString strConflictId;
			const KFileTransferConflictResolution resolution = ConflictResolutionFromName(
				arguments.value(QStringLiteral("resolution")).toString());
			const QJsonValue applyToRemaining = arguments.value(
				QStringLiteral("applyToRemaining"));
			if (!ReadRequiredString(arguments, QStringLiteral("conflictId"), &strConflictId)
				|| resolution == InvalidFileTransferConflictResolution
				|| (!applyToRemaining.isUndefined() && !applyToRemaining.isBool()))
			{
				return InvalidArgumentResult(QStringLiteral("Conflict resolution is invalid"));
			}
			callbacks.resolveFileTransferConflict(strConflictId,
				resolution, applyToRemaining.toBool(false));
			return SucceededResult();
		})
		|| !RegisterCommand(pRegistry, QStringLiteral("file_transfer.clear_completed"),
		[callbacks](const QJsonObject &)
		{
			callbacks.clearCompletedFileTransferTasks();
			return SucceededResult();
		}))
	{
		return false;
	}

	return true;
}
