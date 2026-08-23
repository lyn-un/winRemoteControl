#include "adapters/windows/file_transfer/windowsfilesystemadapter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <iostream>

namespace
{
	constexpr DWORD kAllowUnprivilegedSymbolicLinkCreation = 0x2;
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	bool WriteFileData(const QString &strPath, const QByteArray &data)
	{
		QFile file(strPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			qCritical().noquote() << QStringLiteral("Fixture open failed for %1: %2")
				.arg(strPath, file.errorString());
			return false;
		}
		if (file.write(data) != data.size() || !file.flush())
		{
			qCritical().noquote() << QStringLiteral("Fixture write failed for %1: %2")
				.arg(strPath, file.errorString());
			return false;
		}
		file.close();
		return true;
	}

	QByteArray ReadFileData(const QString &strPath)
	{
		QFile file(strPath);
		if (!file.open(QIODevice::ReadOnly))
			return QByteArray();
		return file.readAll();
	}

	KFileListingEntry FindEntry(const QVector<KFileListingEntry> &entries,
		const QString &strName)
	{
		for (const KFileListingEntry &entry : entries)
		{
			if (entry.strName == strName)
				return entry;
		}
		return KFileListingEntry();
	}

	KFileTreeFileEntry FindTreeFile(const KFileTreeExpansion &expansion,
		const QString &strRelativePath)
	{
		for (const KFileTreeFileEntry &entry : expansion.fileList)
		{
			if (entry.strRelativePath == strRelativePath)
				return entry;
		}
		return KFileTreeFileEntry();
	}

	KRelativeDirectoryEntry FindPreparedDirectory(
		const KDirectoryPreparationResult &result,
		const QString &strRelativePath)
	{
		for (const KRelativeDirectoryEntry &entry : result.directoryList)
		{
			if (entry.strRelativePath == strRelativePath)
				return entry;
		}
		return KRelativeDirectoryEntry();
	}

	QStringList TransferTemporaryFiles(const QString &strDirectory)
	{
		return QDir(strDirectory).entryList(
			{QStringLiteral("*.wrc-part")},
			QDir::Files | QDir::Hidden | QDir::System);
	}

	void TestDriveEnumeration(KWindowsFileSystemAdapter *pAdapter)
	{
		QVector<KFileListingEntry> drives;
		QString strError;
		Check(pAdapter->listDrives(&drives, &strError),
			QStringLiteral("drive enumeration succeeds: %1").arg(strError));
		Check(!drives.isEmpty(), QStringLiteral("at least one drive root is returned"));
		for (const KFileListingEntry &drive : drives)
		{
			Check(drive.type == DriveFileListingEntryType,
				QStringLiteral("drive entries use the drive type"));
			Check(drive.strName.size() == 2
					&& drive.strName.at(0).isLetter()
					&& drive.strName.at(1) == QLatin1Char(':'),
				QStringLiteral("drive names expose only a drive letter"));
			Check(!drive.strOpaqueId.contains(drive.strName, Qt::CaseInsensitive),
				QStringLiteral("drive identifiers remain opaque"));
		}
	}

	void TestListingAndSourceSnapshot(KWindowsFileSystemAdapter *pAdapter,
		const QString &strRootPath,
		const QByteArray &sourceData,
		KFileListingEntry *pDestinationDirectory)
	{
		KFileListingEntry root;
		QString strError;
		Check(pAdapter->createLocalReference(strRootPath, &root, &strError),
			QStringLiteral("trusted local directory is canonicalized: %1").arg(strError));
		Check(root.type == DirectoryFileListingEntryType,
			QStringLiteral("temporary root is a directory reference"));
		Check(!root.strOpaqueId.contains(strRootPath, Qt::CaseInsensitive),
			QStringLiteral("local directory reference does not disclose its path"));

		QVector<KFileListingEntry> entries;
		Check(pAdapter->listDirectory(root.strOpaqueId, &entries, &strError),
			QStringLiteral("directory enumeration succeeds: %1").arg(strError));
		const KFileListingEntry source = FindEntry(entries, QStringLiteral("report.txt"));
		*pDestinationDirectory = FindEntry(entries, QStringLiteral("destination"));
		Check(source.type == RegularFileListingEntryType,
			QStringLiteral("regular source file is listed"));
		Check(source.nSize == static_cast<quint64>(sourceData.size()),
			QStringLiteral("listing reports the source size"));
		Check(pDestinationDirectory->type == DirectoryFileListingEntryType,
			QStringLiteral("destination directory is listed"));
		Check(FindEntry(entries, QStringLiteral(".orphan.wrc-part")).strOpaqueId.isEmpty(),
			QStringLiteral("transfer temporary files are hidden from listings"));

		KFileSourceSnapshot snapshot;
		Check(pAdapter->openSourceSnapshot(source.strOpaqueId, &snapshot, &strError),
			QStringLiteral("source snapshot opens: %1").arg(strError));
		Check(snapshot.strName == QStringLiteral("report.txt"),
			QStringLiteral("source snapshot keeps the leaf name"));
		Check(snapshot.nSize == static_cast<quint64>(sourceData.size()),
			QStringLiteral("source snapshot keeps the size"));
		Check(snapshot.lastModifiedUtc.isValid(),
			QStringLiteral("source snapshot includes last-modified metadata"));

		QByteArray received;
		quint64 nOffset = 0;
		bool bComplete = false;
		while (!bComplete)
		{
			QByteArray chunk;
			Check(pAdapter->readSourceChunk(snapshot.strSourceId, nOffset, 5,
					&chunk, &bComplete, &strError),
				QStringLiteral("source chunks can be read by offset: %1").arg(strError));
			if (!strError.isEmpty())
				break;
			received.append(chunk);
			nOffset += static_cast<quint64>(chunk.size());
		}
		Check(received == sourceData, QStringLiteral("source chunks reconstruct the file"));
		QByteArray beyondData;
		Check(!pAdapter->readSourceChunk(snapshot.strSourceId, snapshot.nSize + 1, 1,
				&beyondData, &bComplete, &strError),
			QStringLiteral("source reads beyond the snapshot are rejected"));
		Check(pAdapter->closeSourceSnapshot(snapshot.strSourceId, &strError),
			QStringLiteral("source snapshot closes: %1").arg(strError));
		Check(!pAdapter->closeSourceSnapshot(snapshot.strSourceId, &strError),
			QStringLiteral("source snapshot cannot be closed twice"));
	}

	void TestKeepBothAndFinalize(KWindowsFileSystemAdapter *pAdapter,
		const KFileListingEntry &destinationDirectory,
		const QString &strDestinationPath,
		const QByteArray &sourceData)
	{
		QString strError;
		KFileWriteRequest request;
		request.strDirectoryOpaqueId = destinationDirectory.strOpaqueId;
		request.strFileName = QStringLiteral("report.txt");
		request.nExpectedSize = static_cast<quint64>(sourceData.size());
		request.lastModifiedUtc = QDateTime::currentDateTimeUtc().addDays(-1);
		request.collisionPolicy = KeepBothFileCollisionPolicy;

		KFileWriteSession session;
		Check(pAdapter->beginWrite(request, &session, &strError),
			QStringLiteral("keep-both write begins: %1").arg(strError));
		Check(session.strProposedFileName == QStringLiteral("report (2).txt"),
			QStringLiteral("keep-both proposes the next available suffix"));
		Check(TransferTemporaryFiles(strDestinationPath).size() == 1,
			QStringLiteral("write uses exactly one .wrc-part file"));
		Check(pAdapter->appendWriteChunk(session.strWriteId, sourceData.left(7), &strError),
			QStringLiteral("first destination chunk is written: %1").arg(strError));
		Check(pAdapter->appendWriteChunk(session.strWriteId, sourceData.mid(7), &strError),
			QStringLiteral("second destination chunk is written: %1").arg(strError));

		const QByteArray digest = QCryptographicHash::hash(
			sourceData, QCryptographicHash::Sha256);
		KFileWriteResult result;
		Check(pAdapter->finalizeWrite(session.strWriteId, digest, &result, &strError),
			QStringLiteral("SHA-256 verified write finalizes: %1").arg(strError));
		Check(result.strFileName == QStringLiteral("report (2).txt"),
			QStringLiteral("final keep-both name is reported"));
		Check(result.nBytesWritten == static_cast<quint64>(sourceData.size())
				&& result.sha256 == digest,
			QStringLiteral("final result reports byte count and SHA-256"));
		Check(ReadFileData(QDir(strDestinationPath).filePath(result.strFileName)) == sourceData,
			QStringLiteral("final destination contains the transferred bytes"));
		Check(ReadFileData(QDir(strDestinationPath).filePath(QStringLiteral("report.txt")))
				== QByteArray("existing"),
			QStringLiteral("keep-both preserves the existing destination"));
		Check(TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("successful finalize removes the temporary name"));
	}

	void TestDestinationInspectionAndOverwrite(KWindowsFileSystemAdapter *pAdapter,
		const KFileListingEntry &destinationDirectory,
		const QString &strDestinationPath,
		const QByteArray &sourceData)
	{
		QString strError;
		bool bExists = false;
		Check(pAdapter->destinationExists(destinationDirectory.strOpaqueId,
				QStringLiteral("overwrite.txt"), &bExists, &strError)
				&& bExists,
			QStringLiteral("destination inspection reports a conflict: %1").arg(strError));
		Check(pAdapter->destinationExists(destinationDirectory.strOpaqueId,
				QStringLiteral("available.txt"), &bExists, &strError)
				&& !bExists,
			QStringLiteral("destination inspection reports an available name: %1").arg(strError));
		Check(!pAdapter->destinationExists(destinationDirectory.strOpaqueId,
				QStringLiteral("unsafe.txt:stream"), &bExists, &strError),
			QStringLiteral("destination inspection rejects unsafe names"));

		KFileWriteRequest request;
		request.strDirectoryOpaqueId = destinationDirectory.strOpaqueId;
		request.strFileName = QStringLiteral("overwrite.txt");
		request.nExpectedSize = static_cast<quint64>(sourceData.size());
		request.collisionPolicy = ReplaceExistingFileCollisionPolicy;
		KFileWriteSession session;
		Check(pAdapter->beginWrite(request, &session, &strError),
			QStringLiteral("overwrite write begins: %1").arg(strError));
		Check(pAdapter->appendWriteChunk(session.strWriteId, sourceData, &strError),
			QStringLiteral("overwrite bytes are staged: %1").arg(strError));
		Check(ReadFileData(QDir(strDestinationPath).filePath(
				QStringLiteral("overwrite.txt"))) == QByteArray("old-overwrite"),
			QStringLiteral("old destination remains until verification succeeds"));
		KFileWriteResult result;
		const QByteArray digest = QCryptographicHash::hash(
			sourceData, QCryptographicHash::Sha256);
		const bool bFinalized = pAdapter->finalizeWrite(
			session.strWriteId, digest, &result, &strError);
		if (!bFinalized)
			qCritical().noquote() << QStringLiteral("Overwrite finalize error: %1").arg(strError);
		Check(bFinalized,
			QStringLiteral("verified overwrite finalizes atomically: %1").arg(strError));
		Check(result.strFileName == QStringLiteral("overwrite.txt")
				&& ReadFileData(QDir(strDestinationPath).filePath(result.strFileName))
					== sourceData,
			QStringLiteral("verified overwrite replaces the old destination"));

		KFileWriteRequest protectedRequest = request;
		protectedRequest.strFileName = QStringLiteral("protected.txt");
		KFileWriteSession protectedSession;
		Check(pAdapter->beginWrite(protectedRequest, &protectedSession, &strError),
			QStringLiteral("protected overwrite begins: %1").arg(strError));
		Check(pAdapter->appendWriteChunk(protectedSession.strWriteId, sourceData, &strError),
			QStringLiteral("protected overwrite bytes are staged: %1").arg(strError));
		Check(!pAdapter->finalizeWrite(protectedSession.strWriteId,
				QByteArray(32, '\0'), &result, &strError),
			QStringLiteral("invalid digest rejects overwrite"));
		Check(ReadFileData(QDir(strDestinationPath).filePath(
				QStringLiteral("protected.txt"))) == QByteArray("protected-old"),
			QStringLiteral("failed verification preserves the old destination"));
		Check(TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("failed overwrite removes its temporary file"));
	}

	void TestFailureCleanupAndAbort(KWindowsFileSystemAdapter *pAdapter,
		const KFileListingEntry &destinationDirectory,
		const QString &strDestinationPath,
		const QByteArray &sourceData)
	{
		QString strError;
		KFileWriteRequest badDigestRequest;
		badDigestRequest.strDirectoryOpaqueId = destinationDirectory.strOpaqueId;
		badDigestRequest.strFileName = QStringLiteral("bad-digest.bin");
		badDigestRequest.nExpectedSize = static_cast<quint64>(sourceData.size());
		badDigestRequest.collisionPolicy = RejectExistingFileCollisionPolicy;
		KFileWriteSession badDigestSession;
		Check(pAdapter->beginWrite(badDigestRequest, &badDigestSession, &strError),
			QStringLiteral("digest failure write begins: %1").arg(strError));
		Check(pAdapter->appendWriteChunk(badDigestSession.strWriteId, sourceData, &strError),
			QStringLiteral("digest failure bytes are staged: %1").arg(strError));
		KFileWriteResult result;
		Check(!pAdapter->finalizeWrite(badDigestSession.strWriteId,
				QByteArray(32, '\0'), &result, &strError),
			QStringLiteral("SHA-256 mismatch rejects finalization"));
		Check(!QFile::exists(QDir(strDestinationPath).filePath(
				QStringLiteral("bad-digest.bin"))),
			QStringLiteral("digest mismatch does not publish a destination file"));
		Check(TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("digest mismatch removes its temporary file"));

		KFileWriteRequest abortRequest = badDigestRequest;
		abortRequest.strFileName = QStringLiteral("aborted.bin");
		KFileWriteSession abortSession;
		Check(pAdapter->beginWrite(abortRequest, &abortSession, &strError),
			QStringLiteral("abort write begins: %1").arg(strError));
		Check(pAdapter->appendWriteChunk(abortSession.strWriteId,
			sourceData.left(3), &strError),
			QStringLiteral("abort write stages bytes: %1").arg(strError));
		Check(pAdapter->abortWrite(abortSession.strWriteId, &strError),
			QStringLiteral("write abort succeeds: %1").arg(strError));
		Check(!QFile::exists(QDir(strDestinationPath).filePath(QStringLiteral("aborted.bin")))
				&& TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("abort removes all write artifacts"));

		KFileWriteRequest rejectRequest = badDigestRequest;
		rejectRequest.strFileName = QStringLiteral("report.txt");
		KFileWriteSession rejectSession;
		Check(!pAdapter->beginWrite(rejectRequest, &rejectSession, &strError),
			QStringLiteral("reject collision policy preserves an existing file"));
		Check(TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("rejected collision creates no temporary file"));

		for (const QString &strUnsafeName : {
			QStringLiteral("..\\escape.bin"),
			QStringLiteral("stream.bin:secret"),
			QStringLiteral("CON.txt"),
			QStringLiteral("reserved.wrc-part")})
		{
			KFileWriteRequest unsafeRequest = badDigestRequest;
			unsafeRequest.strFileName = strUnsafeName;
			KFileWriteSession unsafeSession;
			Check(!pAdapter->beginWrite(unsafeRequest, &unsafeSession, &strError),
				QStringLiteral("unsafe destination name is rejected: %1").arg(strUnsafeName));
		}
	}

	void TestWriteCancellationPhase(KWindowsFileSystemAdapter *pAdapter,
		const KFileListingEntry &destinationDirectory,
		const QString &strDestinationPath,
		const QByteArray &sourceData)
	{
		QString strError;
		KFileWriteRequest request;
		request.strDirectoryOpaqueId = destinationDirectory.strOpaqueId;
		request.strFileName = QStringLiteral("cancel-before-commit.bin");
		request.nExpectedSize = static_cast<quint64>(sourceData.size());
		request.collisionPolicy = RejectExistingFileCollisionPolicy;
		KFileWriteSession session;
		Check(pAdapter->beginWrite(request, &session, &strError),
			QStringLiteral("cancellable write begins: %1").arg(strError));
		Check(pAdapter->appendWriteChunk(session.strWriteId, sourceData, &strError),
			QStringLiteral("cancellable write stages bytes: %1").arg(strError));
		Check(pAdapter->tryRequestWriteCancellation(session.strWriteId)
				&& pAdapter->tryRequestWriteCancellation(session.strWriteId),
			QStringLiteral("write cancellation request is idempotent before commit"));
		KFileWriteResult result;
		const QByteArray digest = QCryptographicHash::hash(
			sourceData, QCryptographicHash::Sha256);
		Check(!pAdapter->finalizeWrite(session.strWriteId, digest, &result, &strError),
			QStringLiteral("a cancellation request prevents final publication"));
		Check(!QFile::exists(QDir(strDestinationPath).filePath(request.strFileName))
				&& TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("cancelled finalization leaves no destination or part file"));
		Check(!pAdapter->tryRequestWriteCancellation(session.strWriteId),
			QStringLiteral("completed cancellation state is no longer addressable"));
	}

	void TestRecursiveDirectoryOperations(KWindowsFileSystemAdapter *pAdapter,
		const QString &strRootPath)
	{
		const QString strSourceTree = QDir(strRootPath).filePath(
			QStringLiteral("tree-source"));
		const QString strDeepSource = QDir(strSourceTree).filePath(
			QStringLiteral("sub/deep"));
		const QString strEmptySource = QDir(strSourceTree).filePath(
			QStringLiteral("empty"));
		const QString strDestinationTree = QDir(strRootPath).filePath(
			QStringLiteral("tree-destination"));
		Check(QDir().mkpath(strDeepSource) && QDir().mkpath(strEmptySource)
				&& QDir().mkpath(QDir(strDestinationTree).filePath(QStringLiteral("existing"))),
			QStringLiteral("recursive directory fixtures are created"));
		Check(WriteFileData(QDir(strSourceTree).filePath(QStringLiteral("root.txt")),
				QByteArray("root"))
				&& WriteFileData(QDir(strDeepSource).filePath(QStringLiteral("leaf.bin")),
					QByteArray("leaf-data")),
			QStringLiteral("recursive file fixtures are written"));

		QString strError;
		KFileListingEntry sourceRoot;
		Check(pAdapter->createLocalReference(strSourceTree, &sourceRoot, &strError),
			QStringLiteral("recursive source root is referenced: %1").arg(strError));
		KFileTreeExpansion expansion;
		Check(pAdapter->expandDirectoryTree(sourceRoot.strOpaqueId, 100,
				&expansion, &strError),
			QStringLiteral("directory tree expands safely: %1").arg(strError));
		Check(expansion.relativeDirectoryPathList
				== QStringList({QStringLiteral("empty"), QStringLiteral("sub"),
					QStringLiteral("sub/deep")}),
			QStringLiteral("directory expansion preserves nested and empty directories"));
		Check(expansion.fileList.size() == 2,
			QStringLiteral("directory expansion returns all regular files"));
		const KFileTreeFileEntry leafFile = FindTreeFile(
			expansion, QStringLiteral("sub/deep/leaf.bin"));
		Check(!leafFile.strOpaqueId.isEmpty() && leafFile.nSize == 9
				&& leafFile.lastModifiedUtc.isValid(),
			QStringLiteral("expanded files include opaque ID, size and mtime"));
		KFileSourceSnapshot leafSnapshot;
		Check(pAdapter->openSourceSnapshot(leafFile.strOpaqueId, &leafSnapshot, &strError),
			QStringLiteral("expanded file opaque ID opens a source snapshot: %1").arg(strError));
		Check(pAdapter->closeSourceSnapshot(leafSnapshot.strSourceId, &strError),
			QStringLiteral("expanded file source snapshot closes: %1").arg(strError));

		KFileTreeExpansion limitedExpansion;
		Check(!pAdapter->expandDirectoryTree(sourceRoot.strOpaqueId, 1,
				&limitedExpansion, &strError)
				&& limitedExpansion.fileList.isEmpty()
				&& limitedExpansion.relativeDirectoryPathList.isEmpty(),
			QStringLiteral("bounded expansion fails closed when its entry limit is exceeded"));
		Check(!pAdapter->expandDirectoryTree(sourceRoot.strOpaqueId, 50001,
				&limitedExpansion, &strError),
			QStringLiteral("expansion rejects limits above 50000"));

		KFileListingEntry destinationRoot;
		Check(pAdapter->createLocalReference(strDestinationTree,
				&destinationRoot, &strError),
			QStringLiteral("recursive destination root is referenced: %1").arg(strError));
		QStringList destinationDirectoryPaths = expansion.relativeDirectoryPathList;
		destinationDirectoryPaths.append(QStringLiteral("existing"));
		KDirectoryPreparationResult preparation;
		Check(pAdapter->prepareRelativeDirectories(destinationRoot.strOpaqueId,
				destinationDirectoryPaths, &preparation, &strError),
			QStringLiteral("relative destination directories are prepared: %1").arg(strError));
		Check(preparation.directoryList.size() == destinationDirectoryPaths.size(),
			QStringLiteral("preparation returns a relative path to opaque ID mapping"));
		Check(preparation.createdDirectoryList.size() == 3,
			QStringLiteral("only directories created by this task receive cleanup tokens"));
		const KRelativeDirectoryEntry deepDestination = FindPreparedDirectory(
			preparation, QStringLiteral("sub/deep"));
		const KRelativeDirectoryEntry existingDestination = FindPreparedDirectory(
			preparation, QStringLiteral("existing"));
		Check(!deepDestination.strDirectoryOpaqueId.isEmpty()
				&& !existingDestination.strDirectoryOpaqueId.isEmpty(),
			QStringLiteral("prepared directories have opaque destination IDs"));

		KDirectoryPreparationResult repeatedPreparation;
		Check(pAdapter->prepareRelativeDirectories(destinationRoot.strOpaqueId,
				destinationDirectoryPaths, &repeatedPreparation, &strError)
				&& repeatedPreparation.createdDirectoryList.isEmpty(),
			QStringLiteral("preparing existing relative directories is idempotent"));
		KDirectoryPreparationResult unsafePreparation;
		Check(!pAdapter->prepareRelativeDirectories(destinationRoot.strOpaqueId,
				{QStringLiteral("../escape")}, &unsafePreparation, &strError),
			QStringLiteral("relative directory traversal is rejected"));

		const QString strReceivedFile = QDir(strDestinationTree).filePath(
			QStringLiteral("sub/deep/received.bin"));
		Check(WriteFileData(strReceivedFile, QByteArray("received")),
			QStringLiteral("received file fixture is written"));
		QStringList cleanupTokens;
		for (const KCreatedDirectoryToken &created : preparation.createdDirectoryList)
			cleanupTokens.append(created.strCleanupToken);
		KDirectoryCleanupResult cleanupResult;
		Check(pAdapter->cleanupCreatedDirectories(cleanupTokens,
				&cleanupResult, &strError),
			QStringLiteral("cleanup safely skips non-empty created directories: %1")
				.arg(strError));
		Check(cleanupResult.removedCleanupTokenList.size() == 1
				&& cleanupResult.retainedCleanupTokenList.size() == 2,
			QStringLiteral("cleanup removes only empty task-created directories"));
		Check(QDir(QDir(strDestinationTree).filePath(QStringLiteral("existing"))).exists(),
			QStringLiteral("cleanup never removes a pre-existing directory"));

		Check(QFile::remove(strReceivedFile), QStringLiteral("received fixture is removed"));
		KDirectoryCleanupResult finalCleanup;
		Check(pAdapter->cleanupCreatedDirectories(
				cleanupResult.retainedCleanupTokenList, &finalCleanup, &strError),
			QStringLiteral("retained cleanup tokens can be retried: %1").arg(strError));
		Check(finalCleanup.retainedCleanupTokenList.isEmpty()
				&& !QDir(QDir(strDestinationTree).filePath(QStringLiteral("sub"))).exists(),
			QStringLiteral("retry removes created directories deepest-first"));

		KDirectoryCleanupResult forgedCleanup;
		Check(!pAdapter->cleanupCreatedDirectories(
				{existingDestination.strDirectoryOpaqueId}, &forgedCleanup, &strError)
				&& QDir(QDir(strDestinationTree).filePath(QStringLiteral("existing"))).exists(),
			QStringLiteral("ordinary opaque IDs cannot authorize directory deletion"));
	}

	void TestUnsafePathsAndReparsePoints(KWindowsFileSystemAdapter *pAdapter,
		const QString &strRootPath)
	{
		QString strError;
		KFileListingEntry entry;
		Check(!pAdapter->createLocalReference(QStringLiteral("\\\\server\\share\\file"),
				&entry, &strError),
			QStringLiteral("UNC paths are rejected"));
		Check(!pAdapter->createLocalReference(QStringLiteral("\\\\.\\C:\\Windows"),
				&entry, &strError),
			QStringLiteral("device paths are rejected"));
		Check(!pAdapter->createLocalReference(strRootPath + QStringLiteral(":stream"),
				&entry, &strError),
			QStringLiteral("alternate data streams are rejected"));
		Check(!pAdapter->createLocalReference(
				QDir(strRootPath).filePath(QStringLiteral("destination/../destination")),
				&entry, &strError),
			QStringLiteral("path traversal components are rejected before canonicalization"));

		const QString strTargetPath = QDir(strRootPath).filePath(QStringLiteral("link-target"));
		const QString strLinkPath = QDir(strRootPath).filePath(QStringLiteral("link"));
		QDir().mkpath(strTargetPath);
		const DWORD nFlags = SYMBOLIC_LINK_FLAG_DIRECTORY
			| kAllowUnprivilegedSymbolicLinkCreation;
		const bool bLinkCreated = CreateSymbolicLinkW(
			reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(strLinkPath).utf16()),
			reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(strTargetPath).utf16()),
			nFlags) != FALSE;
		if (!bLinkCreated)
		{
			qInfo() << "SKIPPED: symbolic-link creation is unavailable";
			return;
		}
		Check(!pAdapter->createLocalReference(strLinkPath, &entry, &strError),
			QStringLiteral("direct references to reparse points are rejected"));
		KFileListingEntry root;
		Check(pAdapter->createLocalReference(strRootPath, &root, &strError),
			QStringLiteral("root can be reopened after link creation: %1").arg(strError));
		QVector<KFileListingEntry> entries;
		Check(pAdapter->listDirectory(root.strOpaqueId, &entries, &strError),
			QStringLiteral("root containing a link can be listed: %1").arg(strError));
		Check(FindEntry(entries, QStringLiteral("link")).strOpaqueId.isEmpty(),
			QStringLiteral("reparse points are omitted from directory listings"));
		KFileTreeExpansion expansion;
		Check(!pAdapter->expandDirectoryTree(root.strOpaqueId, 100,
				&expansion, &strError),
			QStringLiteral("recursive expansion rejects a tree containing a reparse point"));
		RemoveDirectoryW(reinterpret_cast<LPCWSTR>(
			QDir::toNativeSeparators(strLinkPath).utf16()));
	}

	void TestAdapterDestructionAbortsWrites(const QString &strDestinationPath)
	{
		QString strError;
		{
			KWindowsFileSystemAdapter adapter;
			KFileListingEntry directory;
			Check(adapter.createLocalReference(strDestinationPath, &directory, &strError),
				QStringLiteral("destructor test directory is referenced: %1").arg(strError));
			KFileWriteRequest request;
			request.strDirectoryOpaqueId = directory.strOpaqueId;
			request.strFileName = QStringLiteral("abandoned.bin");
			request.nExpectedSize = 10;
			KFileWriteSession session;
			Check(adapter.beginWrite(request, &session, &strError),
				QStringLiteral("destructor test write begins: %1").arg(strError));
			Check(adapter.appendWriteChunk(session.strWriteId, QByteArray("partial"), &strError),
				QStringLiteral("destructor test stages data: %1").arg(strError));
		}
		Check(!QFile::exists(QDir(strDestinationPath).filePath(
				QStringLiteral("abandoned.bin")))
				&& TransferTemporaryFiles(strDestinationPath).isEmpty(),
			QStringLiteral("adapter destruction aborts unfinished writes"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QTemporaryDir temporaryDirectory;
	Check(temporaryDirectory.isValid(), QStringLiteral("isolated temporary directory is available"));
	if (!temporaryDirectory.isValid())
		return 1;

	const QString strRootPath = temporaryDirectory.path();
	const QString strDestinationPath = QDir(strRootPath).filePath(QStringLiteral("destination"));
	const QByteArray sourceData("hello remote copy\n");
	Check(QDir().mkpath(strDestinationPath), QStringLiteral("destination fixture directory is created"));
	Check(WriteFileData(QDir(strRootPath).filePath(QStringLiteral("report.txt")), sourceData),
		QStringLiteral("source fixture is written"));
	Check(WriteFileData(QDir(strRootPath).filePath(QStringLiteral(".orphan.wrc-part")),
		QByteArray("orphan")), QStringLiteral("temporary fixture is written"));
	Check(WriteFileData(QDir(strDestinationPath).filePath(QStringLiteral("report.txt")),
		QByteArray("existing")), QStringLiteral("collision fixture is written"));
	Check(WriteFileData(QDir(strDestinationPath).filePath(QStringLiteral("report (1).txt")),
		QByteArray("existing-one")), QStringLiteral("keep-both fixture is written"));
	Check(WriteFileData(QDir(strDestinationPath).filePath(QStringLiteral("overwrite.txt")),
		QByteArray("old-overwrite")), QStringLiteral("overwrite fixture is written"));
	Check(WriteFileData(QDir(strDestinationPath).filePath(QStringLiteral("protected.txt")),
		QByteArray("protected-old")), QStringLiteral("protected overwrite fixture is written"));

	KWindowsFileSystemAdapter adapter;
	TestDriveEnumeration(&adapter);
	KFileListingEntry destinationDirectory;
	TestListingAndSourceSnapshot(&adapter, strRootPath, sourceData, &destinationDirectory);
	TestKeepBothAndFinalize(&adapter, destinationDirectory, strDestinationPath, sourceData);
	TestDestinationInspectionAndOverwrite(
		&adapter, destinationDirectory, strDestinationPath, sourceData);
	TestFailureCleanupAndAbort(&adapter, destinationDirectory, strDestinationPath, sourceData);
	TestWriteCancellationPhase(&adapter, destinationDirectory, strDestinationPath, sourceData);
	TestRecursiveDirectoryOperations(&adapter, strRootPath);
	TestUnsafePathsAndReparsePoints(&adapter, strRootPath);
	TestAdapterDestructionAbortsWrites(strDestinationPath);
	return g_nFailureCount == 0 ? 0 : 1;
}
