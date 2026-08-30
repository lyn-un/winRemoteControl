#ifndef _WINREMOTECONTROL_AUTOMATIONPLUGINLOADER_H_
#define _WINREMOTECONTROL_AUTOMATIONPLUGINLOADER_H_

#include "automation/wrcdriverhostapi.h"

#include <QtCore/QLibrary>
#include <QtCore/QString>

class KAutomationPluginLoader
{
public:
	KAutomationPluginLoader();
	~KAutomationPluginLoader();

	KAutomationPluginLoader(const KAutomationPluginLoader &) = delete;
	KAutomationPluginLoader &operator=(const KAutomationPluginLoader &) = delete;

	bool load(const QString &strApplicationDirectory,
		const KWrcDriverHostApiV2 *pHostApi,
		QString *pErrorMessage);
	void shutdown();
	bool isLoaded() const;

private:
	bool validateBuildInfo(const KWrcDriverBuildInfoV2 &buildInfo,
		QString *pErrorMessage) const;

	QLibrary m_library;
	KWrcDriverShutdownFunction m_pShutdown = nullptr;
	bool m_bStarted = false;
};

#endif // _WINREMOTECONTROL_AUTOMATIONPLUGINLOADER_H_
