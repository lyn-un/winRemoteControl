#include "core/file_transfer/filetransferstate.h"

QString FileTransferStateName(KFileTransferState state)
{
	switch (state)
	{
	case ClosedFileTransferState:
		return QStringLiteral("Closed");
	case OpeningFileTransferState:
		return QStringLiteral("Opening");
	case WaitingChannelsFileTransferState:
		return QStringLiteral("WaitingChannels");
	case ReadyFileTransferState:
		return QStringLiteral("Ready");
	case ReconnectingFileTransferState:
		return QStringLiteral("Reconnecting");
	case ClosingFileTransferState:
		return QStringLiteral("Closing");
	case FailedFileTransferState:
		return QStringLiteral("Failed");
	default:
		return QStringLiteral("Invalid");
	}
}

QString FileTransferTaskStateName(KFileTransferTaskState state)
{
	switch (state)
	{
	case ScanningFileTransferTaskState:
		return QStringLiteral("Scanning");
	case QueuedFileTransferTaskState:
		return QStringLiteral("Queued");
	case TransferringFileTransferTaskState:
		return QStringLiteral("Transferring");
	case PausedFileTransferTaskState:
		return QStringLiteral("Paused");
	case WaitingConflictFileTransferTaskState:
		return QStringLiteral("WaitingConflict");
	case CompletedFileTransferTaskState:
		return QStringLiteral("Completed");
	case FailedFileTransferTaskState:
		return QStringLiteral("Failed");
	case CancelledFileTransferTaskState:
		return QStringLiteral("Cancelled");
	default:
		return QStringLiteral("Invalid");
	}
}

QString FileTransferTaskKindName(KFileTransferTaskKind kind)
{
	switch (kind)
	{
	case FileFileTransferTaskKind:
		return QStringLiteral("file");
	case FolderFileTransferTaskKind:
		return QStringLiteral("folder");
	case BatchFileTransferTaskKind:
		return QStringLiteral("batch");
	default:
		return QStringLiteral("file");
	}
}
