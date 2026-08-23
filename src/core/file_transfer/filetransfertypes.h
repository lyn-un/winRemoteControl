#ifndef _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILETRANSFERTYPES_H_
#define _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILETRANSFERTYPES_H_

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

enum KFileListingEntryType
{
	InvalidFileListingEntryType,
	DriveFileListingEntryType,
	DirectoryFileListingEntryType,
	RegularFileListingEntryType
};

struct KFileListingEntry
{
	QString strOpaqueId;
	QString strName;
	KFileListingEntryType type = InvalidFileListingEntryType;
	quint64 nSize = 0;
	QDateTime lastModifiedUtc;
};

struct KFileSourceSnapshot
{
	QString strSourceId;
	QString strName;
	quint64 nSize = 0;
	QDateTime lastModifiedUtc;
};

struct KFileTreeFileEntry
{
	QString strRelativePath;
	QString strOpaqueId;
	quint64 nSize = 0;
	QDateTime lastModifiedUtc;
};

struct KFileTreeExpansion
{
	QStringList relativeDirectoryPathList;
	QVector<KFileTreeFileEntry> fileList;
};

struct KRelativeDirectoryEntry
{
	QString strRelativePath;
	QString strDirectoryOpaqueId;
};

struct KCreatedDirectoryToken
{
	QString strRelativePath;
	QString strDirectoryOpaqueId;
	QString strCleanupToken;
};

struct KDirectoryPreparationResult
{
	QVector<KRelativeDirectoryEntry> directoryList;
	QVector<KCreatedDirectoryToken> createdDirectoryList;
};

struct KDirectoryCleanupResult
{
	QStringList removedCleanupTokenList;
	QStringList retainedCleanupTokenList;
};

enum KFileCollisionPolicy
{
	RejectExistingFileCollisionPolicy,
	SkipExistingFileCollisionPolicy = RejectExistingFileCollisionPolicy,
	OverwriteFileCollisionPolicy,
	ReplaceExistingFileCollisionPolicy = OverwriteFileCollisionPolicy,
	KeepBothFileCollisionPolicy
};

struct KFileWriteRequest
{
	QString strDirectoryOpaqueId;
	QString strFileName;
	quint64 nExpectedSize = 0;
	QDateTime lastModifiedUtc;
	KFileCollisionPolicy collisionPolicy = RejectExistingFileCollisionPolicy;
};

struct KFileWriteSession
{
	QString strWriteId;
	QString strProposedFileName;
	quint64 nExpectedSize = 0;
};

struct KFileWriteResult
{
	QString strFileName;
	quint64 nBytesWritten = 0;
	QByteArray sha256;
};

#endif // _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILETRANSFERTYPES_H_
