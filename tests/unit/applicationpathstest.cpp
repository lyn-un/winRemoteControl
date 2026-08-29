#include "common/applicationpaths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QTemporaryDir temporaryDir;
	const QString strRequestedPath = temporaryDir.filePath(QStringLiteral("profile/controlled"));
	QString strError;
	if (!KApplicationPaths::configureDataDirectory(strRequestedPath, &strError))
	{
		qCritical().noquote() << strError;
		return 1;
	}
	if (KApplicationPaths::dataDirectoryPath() != QDir(strRequestedPath).absolutePath())
	{
		qCritical().noquote() << KApplicationPaths::dataDirectoryPath()
			<< QDir(strRequestedPath).absolutePath();
		return 2;
	}
	if (KApplicationPaths::dataFilePath(QStringLiteral("settings.ini"))
		!= QDir(strRequestedPath).filePath(QStringLiteral("settings.ini")))
	{
		qCritical().noquote() << KApplicationPaths::dataFilePath(QStringLiteral("settings.ini"))
			<< QDir(strRequestedPath).filePath(QStringLiteral("settings.ini"));
		return 3;
	}
	qInfo() << "All application paths tests passed";
	return 0;
}
