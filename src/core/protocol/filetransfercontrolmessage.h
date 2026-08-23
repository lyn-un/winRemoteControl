#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERCONTROLMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERCONTROLMESSAGE_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>
#include <QtCore/QtGlobal>

struct KProtocolEnvelope;

enum KFileTransferControlMessageType
{
	InvalidFileTransferControlMessageType,
	ListRootsRequestFileTransferControlMessageType,
	ListRootsResponseFileTransferControlMessageType,
	ListDirectoryRequestFileTransferControlMessageType,
	ListDirectoryResponseFileTransferControlMessageType,
	CopyRequestFileTransferControlMessageType,
	CopyPlanBeginFileTransferControlMessageType,
	CopyPlanDirectoryFileTransferControlMessageType,
	CopyPlanEndFileTransferControlMessageType,
	FileBeginFileTransferControlMessageType,
	AckFileTransferControlMessageType,
	PauseFileTransferControlMessageType,
	ResumeFileTransferControlMessageType,
	CancelFileTransferControlMessageType,
	ConflictFileTransferControlMessageType,
	ConflictResolutionFileTransferControlMessageType,
	FileCompleteFileTransferControlMessageType,
	TaskCompleteFileTransferControlMessageType,
	ErrorFileTransferControlMessageType
};

enum KFileTransferEntryType
{
	InvalidFileTransferEntryType,
	RootFileTransferEntryType,
	DirectoryFileTransferEntryType,
	RegularFileTransferEntryType
};

enum KFileTransferConflictResolution
{
	InvalidFileTransferConflictResolution,
	OverwriteFileTransferConflictResolution,
	SkipFileTransferConflictResolution,
	KeepBothFileTransferConflictResolution
};

enum KFileTransferDirection
{
	InvalidFileTransferDirection,
	UploadFileTransferDirection,
	DownloadFileTransferDirection
};

enum KFileTransferTaskResult
{
	InvalidFileTransferTaskResult,
	CompletedFileTransferTaskResult,
	SkippedFileTransferTaskResult
};

struct KFileTransferEntry
{
	KFileTransferEntryType type = InvalidFileTransferEntryType;
	QString strEntryId;
	QString strName;
	quint64 nSize = 0;
	qint64 nModifiedAtMs = 0;
	bool bNavigable = false;
	bool bTransferable = false;
};

struct KFileTransferControlMessage
{
	KFileTransferControlMessageType type = InvalidFileTransferControlMessageType;
	QString strRequestId;
	QString strTaskId;
	QString strFileId;
	QString strListingId;
	QString strSourceListingId;
	QString strDestinationListingId;
	QString strEntryId;
	QStringList entryIdList;
	QString strDisplayPath;
	bool bCanGoUp = false;
	QString strNextPageToken;
	bool bHasMore = false;
	QString strFileName;
	QString strRelativePath;
	qint64 nModifiedAtMs = 0;
	QByteArray sha256;
	QString strConflictId;
	bool bApplyToRemaining = false;
	QString strErrorCode;
	quint64 nOffset = 0;
	quint64 nSize = 0;
	quint64 nItemCount = 0;
	KFileTransferConflictResolution conflictResolution =
		InvalidFileTransferConflictResolution;
	KFileTransferDirection direction = InvalidFileTransferDirection;
	KFileTransferTaskResult taskResult = InvalidFileTransferTaskResult;
	QVector<KFileTransferEntry> entryList;
};

class KFileTransferControlMessageCodec
{
public:
	static constexpr int kMaximumDisplayPathCharacters = 32768;
	static constexpr int kMaximumRelativePathCharacters = 32768;
	static constexpr int kMaximumEntryNameCharacters = 255;
	static constexpr int kMaximumOpaqueTokenCharacters = 256;
	static constexpr int kMaximumEntryCount = 256;
	static constexpr int kMaximumEntryIdCount = 256;
	static constexpr int kMaximumCopyPlanItemCount = 50000;
	static constexpr int kSha256Bytes = 32;

	static QString encode(const KFileTransferControlMessage &message);
	static bool decode(const QString &strMessage,
		KFileTransferControlMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KFileTransferControlMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KFileTransferControlMessageType type);
	static QString entryTypeName(KFileTransferEntryType type);
	static QString conflictResolutionName(KFileTransferConflictResolution resolution);
	static QString directionName(KFileTransferDirection direction);
	static QString taskResultName(KFileTransferTaskResult result);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERCONTROLMESSAGE_H_
