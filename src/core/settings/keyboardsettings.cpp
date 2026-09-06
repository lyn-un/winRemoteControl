#include "core/settings/keyboardsettings.h"

namespace
{
	constexpr int kMaximumLocalShortcutCount = 32;
}

KKeyboardSettings SanitizeKeyboardSettings(const KKeyboardSettings &settings)
{
	KKeyboardSettings sanitized;
	const KKeyboardShortcut releaseShortcut = DefaultReleaseKeyboardCaptureShortcut();
	if (settings.toggleFullscreenShortcut.isValid()
		&& !IsSecureAttentionShortcut(settings.toggleFullscreenShortcut)
		&& !(settings.toggleFullscreenShortcut == releaseShortcut))
	{
		sanitized.toggleFullscreenShortcut = settings.toggleFullscreenShortcut;
	}
	for (const KKeyboardShortcut &shortcut : settings.localOnlyShortcuts)
	{
		if (sanitized.localOnlyShortcuts.size() >= kMaximumLocalShortcutCount)
			break;
		if (!shortcut.isValid() || IsSecureAttentionShortcut(shortcut)
			|| shortcut == releaseShortcut
			|| shortcut == sanitized.toggleFullscreenShortcut
			|| sanitized.localOnlyShortcuts.contains(shortcut))
		{
			continue;
		}
		sanitized.localOnlyShortcuts.append(shortcut);
	}
	return sanitized;
}
