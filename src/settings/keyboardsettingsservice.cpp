#include "settings/keyboardsettingsservice.h"

#include "core/settings/keyboardsettingsstore.h"

#include <utility>

namespace
{
	constexpr int kMaximumLocalShortcutCount = 32;
}

KKeyboardSettingsService::KKeyboardSettingsService(
	std::unique_ptr<KKeyboardSettingsStore> spStore,
	QObject *pParent)
	: QObject(pParent)
	, m_spStore(std::move(spStore))
{
	Q_ASSERT(m_spStore != nullptr);
}

KKeyboardSettingsService::~KKeyboardSettingsService()
{
}

void KKeyboardSettingsService::initialize()
{
	KKeyboardSettings loadedSettings;
	QString strError;
	if (!m_spStore->loadSettings(&loadedSettings, &strError))
	{
		emit settingsError(strError);
		return;
	}
	m_settings = SanitizeKeyboardSettings(loadedSettings);
}

KKeyboardSettings KKeyboardSettingsService::settings() const
{
	return m_settings;
}

void KKeyboardSettingsService::requestSettings()
{
	emit settingsChanged(m_settings);
}

void KKeyboardSettingsService::addLocalShortcut(const QString &strShortcut)
{
	KKeyboardShortcut shortcut;
	if (!parseCandidateShortcut(strShortcut, &shortcut))
		return;
	if (m_settings.localOnlyShortcuts.size() >= kMaximumLocalShortcutCount)
	{
		emit settingsError(QStringLiteral("本地快捷键最多只能添加 32 个"));
		return;
	}
	if (shortcut == DefaultReleaseKeyboardCaptureShortcut()
		|| shortcut == m_settings.toggleFullscreenShortcut
		|| m_settings.localOnlyShortcuts.contains(shortcut))
	{
		emit settingsError(QStringLiteral("快捷键重复或与功能快捷键冲突"));
		return;
	}
	KKeyboardSettings candidate = m_settings;
	candidate.localOnlyShortcuts.append(shortcut);
	saveCandidate(candidate);
}

void KKeyboardSettingsService::removeLocalShortcut(const QString &strShortcut)
{
	KKeyboardShortcut shortcut;
	if (!KeyboardShortcutFromCanonicalName(strShortcut, &shortcut))
	{
		emit settingsError(QStringLiteral("快捷键格式无效"));
		return;
	}
	KKeyboardSettings candidate = m_settings;
	if (!candidate.localOnlyShortcuts.removeOne(shortcut))
	{
		emit settingsError(QStringLiteral("没有找到要删除的快捷键"));
		return;
	}
	saveCandidate(candidate);
}

void KKeyboardSettingsService::updateToggleFullscreenShortcut(
	const QString &strShortcut)
{
	KKeyboardShortcut shortcut;
	if (!parseCandidateShortcut(strShortcut, &shortcut))
		return;
	if (shortcut == DefaultReleaseKeyboardCaptureShortcut()
		|| m_settings.localOnlyShortcuts.contains(shortcut))
	{
		emit settingsError(QStringLiteral("快捷键与现有快捷键冲突"));
		return;
	}
	KKeyboardSettings candidate = m_settings;
	candidate.toggleFullscreenShortcut = shortcut;
	saveCandidate(candidate);
}

void KKeyboardSettingsService::restoreDefaults()
{
	saveCandidate(KKeyboardSettings());
}

bool KKeyboardSettingsService::saveCandidate(const KKeyboardSettings &candidate)
{
	QString strError;
	if (!m_spStore->saveSettings(candidate, &strError))
	{
		emit settingsError(strError);
		return false;
	}
	m_settings = candidate;
	emit settingsChanged(m_settings);
	return true;
}

bool KKeyboardSettingsService::parseCandidateShortcut(
	const QString &strShortcut,
	KKeyboardShortcut *pShortcut)
{
	KKeyboardShortcut shortcut;
	if (!KeyboardShortcutFromCanonicalName(strShortcut, &shortcut)
		|| shortcut.modifiers == NoKeyboardShortcutModifier)
	{
		emit settingsError(QStringLiteral("快捷键必须包含修饰键和一个普通按键"));
		return false;
	}
	if (IsSecureAttentionShortcut(shortcut))
	{
		emit settingsError(QStringLiteral("Windows 不允许捕获 Ctrl+Alt+Delete"));
		return false;
	}
	if (pShortcut != nullptr)
		*pShortcut = shortcut;
	return true;
}
