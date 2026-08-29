#include "common/applicationpaths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

QString KApplicationPaths::s_strDataDirectoryPath;
bool KApplicationPaths::s_bAutomationTestProfile = false;

bool KApplicationPaths::configureDataDirectory(const QString &strRequestedPath,
	QString *pErrorMessage)
{
	QString strPath = strRequestedPath.trimmed();
	if (strPath.isEmpty())
		strPath = QCoreApplication::applicationDirPath();
	if (strPath.isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Application data directory is unavailable");
		return false;
	}

	QFileInfo pathInfo(strPath);
	if (pathInfo.isRelative())
		strPath = QDir::current().absoluteFilePath(strPath);
	strPath = QDir::cleanPath(strPath);
	QDir directory;
	if (!directory.mkpath(strPath))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Unable to create application data directory: %1")
				.arg(strPath);
		return false;
	}
	if (!QFileInfo(strPath).isDir())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Application data path is not a directory: %1")
				.arg(strPath);
		return false;
	}

	s_strDataDirectoryPath = QDir(strPath).absolutePath();
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KApplicationPaths::dataDirectoryPath()
{
	if (!s_strDataDirectoryPath.isEmpty())
		return s_strDataDirectoryPath;
	return QCoreApplication::applicationDirPath();
}

QString KApplicationPaths::dataFilePath(const QString &strFileName)
{
	return QDir(dataDirectoryPath()).filePath(strFileName);
}

void KApplicationPaths::setAutomationTestProfile(bool bEnabled)
{
	s_bAutomationTestProfile = bEnabled;
}

bool KApplicationPaths::isAutomationTestProfile()
{
	return s_bAutomationTestProfile;
}
