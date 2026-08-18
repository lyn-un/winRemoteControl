#include "adapters/windows/security/atomicjsonfile.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QUuid>

#include <windows.h>
#include <io.h>

namespace
{
	bool FlushFileToDisk(QFile *pFile, QString *pErrorMessage)
	{
		if (pFile == nullptr || pFile->handle() == -1)
			return false;
		const intptr_t nNativeHandle = _get_osfhandle(pFile->handle());
		if (nNativeHandle == -1)
			return false;
		const HANDLE hFile = reinterpret_cast<HANDLE>(nNativeHandle);
		if (FlushFileBuffers(hFile) != FALSE)
			return true;
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = QStringLiteral(
				"Unable to flush atomic temporary file (0x%1)")
				.arg(GetLastError(), 8, 16, QLatin1Char('0'));
		}
		return false;
	}
}

bool KAtomicJsonFile::write(const QString &strFilePath,
	const QByteArray &data,
	QString *pErrorMessage)
{
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	const QString strTemporaryPath = QStringLiteral("%1.%2.tmp")
		.arg(strFilePath, QUuid::createUuid().toString(QUuid::WithoutBraces));
	QFile temporaryFile(strTemporaryPath);
	if (!temporaryFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
		|| temporaryFile.write(data) != data.size()
		|| !temporaryFile.flush()
		|| !FlushFileToDisk(&temporaryFile, pErrorMessage))
	{
		if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
			*pErrorMessage = QStringLiteral("Unable to write atomic temporary file: %1")
				.arg(temporaryFile.errorString());
		temporaryFile.close();
		QFile::remove(strTemporaryPath);
		return false;
	}
	temporaryFile.close();
	const LPCWSTR pTemporaryPath =
		reinterpret_cast<LPCWSTR>(strTemporaryPath.utf16());
	const LPCWSTR pDestinationPath =
		reinterpret_cast<LPCWSTR>(strFilePath.utf16());
	const bool bDestinationExists = GetFileAttributesW(pDestinationPath)
		!= INVALID_FILE_ATTRIBUTES;
	const BOOL bReplaced = bDestinationExists
		? ReplaceFileW(pDestinationPath, pTemporaryPath, nullptr,
			0, nullptr, nullptr)
		: MoveFileExW(pTemporaryPath, pDestinationPath, MOVEFILE_WRITE_THROUGH);
	if (bReplaced)
		return true;
	const DWORD nError = GetLastError();
	QFile::remove(strTemporaryPath);
	if (pErrorMessage != nullptr)
	{
		*pErrorMessage = QStringLiteral("Unable to replace atomic file (0x%1)")
			.arg(nError, 8, 16, QLatin1Char('0'));
	}
	return false;
}
