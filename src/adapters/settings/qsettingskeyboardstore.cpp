#include "adapters/settings/qsettingskeyboardstore.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

KQSettingsKeyboardStore::KQSettingsKeyboardStore(const QString &strFilePath)
	: m_strFilePath(strFilePath)
{
}

KQSettingsKeyboardStore::~KQSettingsKeyboardStore()
{
}

bool KQSettingsKeyboardStore::loadSettings(KKeyboardSettings *pSettings, QString *pError)
{
	if (pSettings == nullptr)
		return false;
	KKeyboardSettings settings;
	QSettings storage(m_strFilePath, QSettings::IniFormat);
	KKeyboardShortcut fullscreenShortcut;
	if (KeyboardShortcutFromCanonicalName(storage.value(
		QStringLiteral("keyboard/toggleFullscreen"),
		KeyboardShortcutCanonicalName(DefaultToggleFullscreenShortcut())).toString(),
		&fullscreenShortcut))
	{
		settings.toggleFullscreenShortcut = fullscreenShortcut;
	}
	const QStringList shortcutNames = storage.value(
		QStringLiteral("keyboard/localOnlyShortcuts")).toStringList();
	for (const QString &strName : shortcutNames)
	{
		KKeyboardShortcut shortcut;
		if (KeyboardShortcutFromCanonicalName(strName, &shortcut))
			settings.localOnlyShortcuts.append(shortcut);
	}
	if (storage.status() != QSettings::NoError)
	{
		if (pError != nullptr)
			*pError = QStringLiteral("读取键盘设置失败");
		return false;
	}
	*pSettings = SanitizeKeyboardSettings(settings);
	return true;
}

bool KQSettingsKeyboardStore::saveSettings(
	const KKeyboardSettings &settings,
	QString *pError)
{
	const QFileInfo fileInfo(m_strFilePath);
	if (!QDir().mkpath(fileInfo.absolutePath()))
	{
		if (pError != nullptr)
			*pError = QStringLiteral("无法创建键盘设置目录");
		return false;
	}
	QStringList shortcutNames;
	for (const KKeyboardShortcut &shortcut : settings.localOnlyShortcuts)
		shortcutNames.append(KeyboardShortcutCanonicalName(shortcut));
	QSettings storage(m_strFilePath, QSettings::IniFormat);
	storage.setValue(QStringLiteral("keyboard/schemaVersion"), 1);
	storage.setValue(QStringLiteral("keyboard/localOnlyShortcuts"), shortcutNames);
	storage.setValue(QStringLiteral("keyboard/toggleFullscreen"),
		KeyboardShortcutCanonicalName(settings.toggleFullscreenShortcut));
	storage.sync();
	if (storage.status() == QSettings::NoError)
		return true;
	if (pError != nullptr)
		*pError = QStringLiteral("写入键盘设置失败");
	return false;
}
