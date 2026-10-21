#include "common/applicationpaths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QTemporaryDir temporaryDir;
	const QString strRequestedPath = temporaryDir.filePath(QStringLiteral("profile/controlled"));
	QString strError;
	if (KApplicationPaths::validateAutomationTestProfile(QString(), true, &strError)
		|| !strError.startsWith(QStringLiteral("automation_profile_required")))
		return 4;
	if (!KApplicationPaths::validateAutomationTestProfile(strRequestedPath, true, &strError))
		return 5;
	const QDir profile(strRequestedPath);
	QDir().mkpath(profile.filePath(QStringLiteral("security")));
	for (const QString &strName : { QStringLiteral("device_identity.json"),
		QStringLiteral("trusted_devices.json") })
	{
		QFile marker(profile.filePath(QStringLiteral("security/%1").arg(strName)));
		if (!marker.open(QIODevice::WriteOnly) || marker.write("preserve") != 8)
			return 6;
		marker.close();
		if (KApplicationPaths::validateAutomationTestProfile(strRequestedPath, true, &strError)
			|| !strError.startsWith(QStringLiteral("automation_profile_reuse")))
			return 7;
		if (!KApplicationPaths::validateAutomationTestProfile(strRequestedPath, false, &strError)
			|| !marker.open(QIODevice::ReadOnly) || marker.readAll() != "preserve")
			return 8;
		marker.close();
		marker.remove();
	}
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
