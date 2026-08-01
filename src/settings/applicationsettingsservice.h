#ifndef _WINREMOTECONTROL_SETTINGS_APPLICATIONSETTINGSSERVICE_H_
#define _WINREMOTECONTROL_SETTINGS_APPLICATIONSETTINGSSERVICE_H_

#include "core/settings/applicationsettings.h"

#include <QtCore/QObject>

#include <memory>

class KApplicationSettingsStore;

class KApplicationSettingsService final : public QObject
{
	Q_OBJECT

public:
	explicit KApplicationSettingsService(std::unique_ptr<KApplicationSettingsStore> spStore,
		QObject *pParent = nullptr);
	~KApplicationSettingsService() override;

	KApplicationSettingsService(const KApplicationSettingsService &) = delete;
	KApplicationSettingsService &operator=(const KApplicationSettingsService &) = delete;

	void initialize();
	KApplicationSettings settings() const;

public slots:
	void requestSettings();
	void updateSettings(bool bRemoteAccessEnabled,
		const QString &strApprovalMode,
		int nApprovalTimeoutSeconds,
		int nDefaultListenPort);

signals:
	void settingsChanged(const KApplicationSettings &settings);
	void settingsError(const QString &strError);

private:
	std::unique_ptr<KApplicationSettingsStore> m_spStore;
	KApplicationSettings m_settings;
};

#endif // _WINREMOTECONTROL_SETTINGS_APPLICATIONSETTINGSSERVICE_H_
