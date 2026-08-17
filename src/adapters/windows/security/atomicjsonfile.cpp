#include "adapters/windows/security/atomicjsonfile.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

#include <windows.h>

bool KAtomicJsonFile::write(const QString &strFilePath,
	const QByteArray &data,
	QString *pErrorMessage)
{
	QSaveFile saveFile(strFilePath);
	saveFile.setDirectWriteFallback(false);
	if (saveFile.open(QIODevice::WriteOnly))
	{
		if (saveFile.write(data) == data.size() && saveFile.commit())
			return true;
		saveFile.cancelWriting();
	}

	const QString strTemporaryPath = QStringLiteral("%1.%2.tmp")
		.arg(strFilePath, QUuid::createUuid().toString(QUuid::WithoutBraces));
	QFile temporaryFile(strTemporaryPath);
	if (!temporaryFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
		|| temporaryFile.write(data) != data.size()
		|| !temporaryFile.flush())
	{
		if (pErrorMessage != nullptr)
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
			REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
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
