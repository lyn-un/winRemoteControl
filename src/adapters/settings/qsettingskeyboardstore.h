#ifndef _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSKEYBOARDSTORE_H_
#define _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSKEYBOARDSTORE_H_

#include "core/settings/keyboardsettingsstore.h"

class KQSettingsKeyboardStore final : public KKeyboardSettingsStore
{
public:
	explicit KQSettingsKeyboardStore(const QString &strFilePath);
	~KQSettingsKeyboardStore() override;

	bool loadSettings(KKeyboardSettings *pSettings, QString *pError) override;
	bool saveSettings(const KKeyboardSettings &settings, QString *pError) override;

private:
	QString m_strFilePath;
};

#endif // _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSKEYBOARDSTORE_H_
