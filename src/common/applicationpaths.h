#ifndef _WINREMOTECONTROL_APPLICATIONPATHS_H_
#define _WINREMOTECONTROL_APPLICATIONPATHS_H_

#include <QtCore/QString>

class KApplicationPaths
{
public:
	static bool configureDataDirectory(const QString &strRequestedPath,
		QString *pErrorMessage);
	static QString dataDirectoryPath();
	static QString dataFilePath(const QString &strFileName);
	static void setAutomationTestProfile(bool bEnabled);
	static bool isAutomationTestProfile();

private:
	static QString s_strDataDirectoryPath;
	static bool s_bAutomationTestProfile;
};

#endif // _WINREMOTECONTROL_APPLICATIONPATHS_H_
