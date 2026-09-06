#ifndef _WINREMOTECONTROL_CORE_SETTINGS_KEYBOARDSETTINGS_H_
#define _WINREMOTECONTROL_CORE_SETTINGS_KEYBOARDSETTINGS_H_

#include "core/input/keyboardshortcut.h"

#include <QtCore/QVector>

struct KKeyboardSettings
{
	QVector<KKeyboardShortcut> localOnlyShortcuts;
	KKeyboardShortcut toggleFullscreenShortcut = DefaultToggleFullscreenShortcut();
};

KKeyboardSettings SanitizeKeyboardSettings(const KKeyboardSettings &settings);

Q_DECLARE_METATYPE(KKeyboardSettings)

#endif // _WINREMOTECONTROL_CORE_SETTINGS_KEYBOARDSETTINGS_H_
