#ifndef _WINREMOTECONTROL_SETTINGS_KEYBOARDSETTINGSSERVICE_H_
#define _WINREMOTECONTROL_SETTINGS_KEYBOARDSETTINGSSERVICE_H_

#include "core/settings/keyboardsettings.h"

#include <QtCore/QObject>

#include <memory>

class KKeyboardSettingsStore;

class KKeyboardSettingsService final : public QObject
{
	Q_OBJECT

public:
	explicit KKeyboardSettingsService(std::unique_ptr<KKeyboardSettingsStore> spStore,
		QObject *pParent = nullptr);
	~KKeyboardSettingsService() override;

	void initialize();
	KKeyboardSettings settings() const;

public slots:
	void requestSettings();
	void addLocalShortcut(const QString &strShortcut);
	void removeLocalShortcut(const QString &strShortcut);
	void updateToggleFullscreenShortcut(const QString &strShortcut);
	void restoreDefaults();

signals:
	void settingsChanged(const KKeyboardSettings &settings);
	void settingsError(const QString &strError);

private:
	bool saveCandidate(const KKeyboardSettings &candidate);
	bool parseCandidateShortcut(const QString &strShortcut,
		KKeyboardShortcut *pShortcut);

	std::unique_ptr<KKeyboardSettingsStore> m_spStore;
	KKeyboardSettings m_settings;
};

#endif // _WINREMOTECONTROL_SETTINGS_KEYBOARDSETTINGSSERVICE_H_
