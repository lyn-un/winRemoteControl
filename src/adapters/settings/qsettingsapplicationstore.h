#ifndef _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSAPPLICATIONSTORE_H_
#define _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSAPPLICATIONSTORE_H_

#include "core/settings/applicationsettingsstore.h"

class KQSettingsApplicationStore final : public KApplicationSettingsStore
{
public:
	explicit KQSettingsApplicationStore(const QString &strFilePath);
	~KQSettingsApplicationStore() override;

	bool loadSettings(KApplicationSettings *pSettings, QString *pError) override;
	bool saveSettings(const KApplicationSettings &settings, QString *pError) override;

private:
	QString m_strFilePath;
};

#endif // _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSAPPLICATIONSTORE_H_
