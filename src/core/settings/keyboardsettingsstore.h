#ifndef _WINREMOTECONTROL_CORE_SETTINGS_KEYBOARDSETTINGSSTORE_H_
#define _WINREMOTECONTROL_CORE_SETTINGS_KEYBOARDSETTINGSSTORE_H_

#include "core/settings/keyboardsettings.h"

class KKeyboardSettingsStore
{
public:
	virtual ~KKeyboardSettingsStore() = default;

	virtual bool loadSettings(KKeyboardSettings *pSettings, QString *pError) = 0;
	virtual bool saveSettings(const KKeyboardSettings &settings, QString *pError) = 0;
};

#endif // _WINREMOTECONTROL_CORE_SETTINGS_KEYBOARDSETTINGSSTORE_H_
