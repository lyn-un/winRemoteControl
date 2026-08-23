#ifndef _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILETRANSFERSTATE_H_
#define _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILETRANSFERSTATE_H_

#include "core/file_transfer/filetransfertypes.h"
#include "core/protocol/filetransfercontrolmessage.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QVector>

enum KFileTransferState
{
	ClosedFileTransferState,
	OpeningFileTransferState,
	WaitingChannelsFileTransferState,
	ReadyFileTransferState,
	ReconnectingFileTransferState,
	ClosingFileTransferState,
	FailedFileTransferState
};

enum KFileTransferPane
{
	LocalFileTransferPane,
	RemoteFileTransferPane
};

enum KFileTransferTaskState
{
	ScanningFileTransferTaskState,
	QueuedFileTransferTaskState,
	TransferringFileTransferTaskState,
	PausedFileTransferTaskState,
	WaitingConflictFileTransferTaskState,
	CompletedFileTransferTaskState,
	FailedFileTransferTaskState,
	CancelledFileTransferTaskState
};

enum KFileTransferTaskKind
{
	FileFileTransferTaskKind,
	FolderFileTransferTaskKind,
	BatchFileTransferTaskKind
};

struct KFileTransferPaneEntry
{
	QString strEntryId;
	QString strName;
	KFileListingEntryType type = InvalidFileListingEntryType;
	quint64 nSize = 0;
	QDateTime lastModifiedUtc;
	bool bNavigable = false;
	bool bTransferable = false;
};

struct KFileTransferPaneSnapshot
{
	KFileTransferPane pane = LocalFileTransferPane;
	QString strRequestId;
	QString strListingId;
	QString strDirectoryId;
	QString strDisplayPath;
	QString strParentDirectoryId;
	bool bCanGoUp = false;
	QVector<KFileTransferPaneEntry> entryList;
};

struct KFileTransferTaskSnapshot
{
	QString strTaskId;
	QString strFileId;
	QString strDisplayName;
	KFileTransferTaskKind kind = FileFileTransferTaskKind;
	KFileTransferDirection direction = InvalidFileTransferDirection;
	KFileTransferTaskState state = QueuedFileTransferTaskState;
	quint64 nBytesTransferred = 0;
	quint64 nBytesTotal = 0;
	QString strErrorCode;
	bool bCanPause = false;
	bool bCanRetry = false;
};

struct KFileTransferConflictSnapshot
{
	QString strConflictId;
	QString strTaskId;
	QString strFileId;
	QString strName;
	quint64 nSourceSize = 0;
	QDateTime sourceLastModifiedUtc;
	quint64 nDestinationSize = 0;
	QDateTime destinationLastModifiedUtc;
	bool bApplyToRemainingAllowed = false;
};

QString FileTransferStateName(KFileTransferState state);
QString FileTransferTaskStateName(KFileTransferTaskState state);
QString FileTransferTaskKindName(KFileTransferTaskKind kind);

#endif // _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILETRANSFERSTATE_H_
