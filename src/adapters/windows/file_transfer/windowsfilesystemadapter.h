#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_FILE_TRANSFER_WINDOWSFILESYSTEMADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_FILE_TRANSFER_WINDOWSFILESYSTEMADAPTER_H_

#include "core/file_transfer/filesystemport.h"

#include <memory>

class KWindowsFileSystemAdapterPrivate;

// Operations are synchronous and thread-safe. Callers should schedule disk I/O
// on a worker thread rather than invoking it from the UI thread.
class KWindowsFileSystemAdapter final : public IKFileSystemPort
{
public:
	KWindowsFileSystemAdapter();
	~KWindowsFileSystemAdapter() override;

	KWindowsFileSystemAdapter(const KWindowsFileSystemAdapter &) = delete;
	KWindowsFileSystemAdapter &operator=(const KWindowsFileSystemAdapter &) = delete;

	bool listDrives(QVector<KFileListingEntry> *pEntries,
		QString *pErrorMessage) override;
	bool listDirectory(const QString &strDirectoryOpaqueId,
		QVector<KFileListingEntry> *pEntries,
		QString *pErrorMessage) override;
	bool expandDirectoryTree(const QString &strDirectoryOpaqueId,
		int nMaximumEntries,
		KFileTreeExpansion *pExpansion,
		QString *pErrorMessage) override;
	bool createLocalReference(const QString &strAbsolutePath,
		KFileListingEntry *pEntry,
		QString *pErrorMessage) override;
	bool destinationExists(const QString &strDirectoryOpaqueId,
		const QString &strFileName,
		bool *pExists,
		QString *pErrorMessage) override;
	bool prepareRelativeDirectories(const QString &strRootDirectoryOpaqueId,
		const QStringList &relativeDirectoryPathList,
		KDirectoryPreparationResult *pResult,
		QString *pErrorMessage) override;
	bool cleanupCreatedDirectories(const QStringList &cleanupTokenList,
		KDirectoryCleanupResult *pResult,
		QString *pErrorMessage) override;

	bool openSourceSnapshot(const QString &strFileOpaqueId,
		KFileSourceSnapshot *pSnapshot,
		QString *pErrorMessage) override;
	bool readSourceChunk(const QString &strSourceId,
		quint64 nOffset,
		int nMaximumBytes,
		QByteArray *pData,
		bool *pComplete,
		QString *pErrorMessage) override;
	bool closeSourceSnapshot(const QString &strSourceId,
		QString *pErrorMessage) override;

	bool beginWrite(const KFileWriteRequest &request,
		KFileWriteSession *pSession,
		QString *pErrorMessage) override;
	bool appendWriteChunk(const QString &strWriteId,
		const QByteArray &data,
		QString *pErrorMessage) override;
	bool finalizeWrite(const QString &strWriteId,
		const QByteArray &expectedSha256,
		KFileWriteResult *pResult,
		QString *pErrorMessage) override;
	bool tryRequestWriteCancellation(const QString &strWriteId) override;
	bool abortWrite(const QString &strWriteId,
		QString *pErrorMessage) override;

private:
	std::unique_ptr<KWindowsFileSystemAdapterPrivate> m_spPrivate;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_FILE_TRANSFER_WINDOWSFILESYSTEMADAPTER_H_
