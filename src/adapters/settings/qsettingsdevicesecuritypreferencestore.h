#ifndef _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSDEVICESECURITYPREFERENCESTORE_H_
#define _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSDEVICESECURITYPREFERENCESTORE_H_

#include "core/settings/devicesecuritypreferencestore.h"

class KQSettingsDeviceSecurityPreferenceStore final
	: public KDeviceSecurityPreferenceStore
{
public:
	explicit KQSettingsDeviceSecurityPreferenceStore(const QString &strFilePath);
	~KQSettingsDeviceSecurityPreferenceStore() override;

	QVector<KDeviceSecurityPreference> load(QString *pError) override;
	bool save(const QVector<KDeviceSecurityPreference> &preferences,
		QString *pError) override;

private:
	QString m_strFilePath;
};

#endif // _WINREMOTECONTROL_ADAPTERS_SETTINGS_QSETTINGSDEVICESECURITYPREFERENCESTORE_H_
