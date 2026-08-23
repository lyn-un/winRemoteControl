#ifndef _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILESYSTEMPORT_H_
#define _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILESYSTEMPORT_H_

#include "core/file_transfer/filetransfertypes.h"

#include <QtCore/QVector>

class IKFileSystemPort
{
public:
	virtual ~IKFileSystemPort() = default;

	virtual bool listDrives(QVector<KFileListingEntry> *pEntries,
		QString *pErrorMessage) = 0;
	virtual bool listDirectory(const QString &strDirectoryOpaqueId,
		QVector<KFileListingEntry> *pEntries,
		QString *pErrorMessage) = 0;
	virtual bool expandDirectoryTree(const QString &strDirectoryOpaqueId,
		int nMaximumEntries,
		KFileTreeExpansion *pExpansion,
		QString *pErrorMessage) = 0;

	// This path-based entry point is for trusted local UI selections only. Remote
	// requests must use opaque identifiers returned by this port.
	virtual bool createLocalReference(const QString &strAbsolutePath,
		KFileListingEntry *pEntry,
		QString *pErrorMessage) = 0;
	virtual bool destinationExists(const QString &strDirectoryOpaqueId,
		const QString &strFileName,
		bool *pExists,
		QString *pErrorMessage) = 0;
	virtual bool prepareRelativeDirectories(const QString &strRootDirectoryOpaqueId,
		const QStringList &relativeDirectoryPathList,
		KDirectoryPreparationResult *pResult,
		QString *pErrorMessage) = 0;
	virtual bool cleanupCreatedDirectories(const QStringList &cleanupTokenList,
		KDirectoryCleanupResult *pResult,
		QString *pErrorMessage) = 0;

	virtual bool openSourceSnapshot(const QString &strFileOpaqueId,
		KFileSourceSnapshot *pSnapshot,
		QString *pErrorMessage) = 0;
	virtual bool readSourceChunk(const QString &strSourceId,
		quint64 nOffset,
		int nMaximumBytes,
		QByteArray *pData,
		bool *pComplete,
		QString *pErrorMessage) = 0;
	virtual bool closeSourceSnapshot(const QString &strSourceId,
		QString *pErrorMessage) = 0;

	virtual bool beginWrite(const KFileWriteRequest &request,
		KFileWriteSession *pSession,
		QString *pErrorMessage) = 0;
	virtual bool appendWriteChunk(const QString &strWriteId,
		const QByteArray &data,
		QString *pErrorMessage) = 0;
	virtual bool finalizeWrite(const QString &strWriteId,
		const QByteArray &expectedSha256,
		KFileWriteResult *pResult,
		QString *pErrorMessage) = 0;
	virtual bool tryRequestWriteCancellation(const QString &strWriteId) = 0;
	virtual bool abortWrite(const QString &strWriteId,
		QString *pErrorMessage) = 0;
};

#endif // _WINREMOTECONTROL_CORE_FILE_TRANSFER_FILESYSTEMPORT_H_
