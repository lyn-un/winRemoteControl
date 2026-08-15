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
	const BOOL bMoved = MoveFileExW(
		reinterpret_cast<LPCWSTR>(strTemporaryPath.utf16()),
		reinterpret_cast<LPCWSTR>(strFilePath.utf16()),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	if (bMoved)
		return true;
	const DWORD nError = GetLastError();
	QFile directFile(strFilePath);
	if (nError == ERROR_ACCESS_DENIED
		&& directFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
		&& directFile.write(data) == data.size()
		&& directFile.flush())
	{
		directFile.close();
		QFile::remove(strTemporaryPath);
		return true;
	}
	QFile::remove(strTemporaryPath);
	if (pErrorMessage != nullptr)
	{
		*pErrorMessage = QStringLiteral("Unable to replace atomic file (0x%1)")
			.arg(nError, 8, 16, QLatin1Char('0'));
	}
	return false;
}
