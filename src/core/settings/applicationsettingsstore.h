#ifndef _WINREMOTECONTROL_CORE_SETTINGS_APPLICATIONSETTINGSSTORE_H_
#define _WINREMOTECONTROL_CORE_SETTINGS_APPLICATIONSETTINGSSTORE_H_

#include "core/settings/applicationsettings.h"

class KApplicationSettingsStore
{
public:
	virtual ~KApplicationSettingsStore() = default;

	virtual bool loadSettings(KApplicationSettings *pSettings, QString *pError) = 0;
	virtual bool saveSettings(const KApplicationSettings &settings, QString *pError) = 0;
};

#endif // _WINREMOTECONTROL_CORE_SETTINGS_APPLICATIONSETTINGSSTORE_H_
