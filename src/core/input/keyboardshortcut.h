#ifndef _WINREMOTECONTROL_CORE_INPUT_KEYBOARDSHORTCUT_H_
#define _WINREMOTECONTROL_CORE_INPUT_KEYBOARDSHORTCUT_H_

#include <QtCore/QMetaType>
#include <QtCore/QString>

enum KKeyboardShortcutModifier
{
	NoKeyboardShortcutModifier = 0x00,
	ControlKeyboardShortcutModifier = 0x01,
	AltKeyboardShortcutModifier = 0x02,
	ShiftKeyboardShortcutModifier = 0x04,
	WindowsKeyboardShortcutModifier = 0x08
};
Q_DECLARE_FLAGS(KKeyboardShortcutModifiers, KKeyboardShortcutModifier)

struct KKeyboardShortcut
{
	KKeyboardShortcutModifiers modifiers;
	quint32 nVirtualKey = 0;

	bool isValid() const;
	bool operator==(const KKeyboardShortcut &other) const;
};

KKeyboardShortcut DefaultReleaseKeyboardCaptureShortcut();
KKeyboardShortcut DefaultToggleFullscreenShortcut();
quint32 NormalizeShortcutVirtualKey(quint32 nVirtualKey);
KKeyboardShortcutModifier ModifierForVirtualKey(quint32 nVirtualKey);
QString KeyboardShortcutCanonicalName(const KKeyboardShortcut &shortcut);
QString KeyboardShortcutDisplayName(const KKeyboardShortcut &shortcut);
bool KeyboardShortcutFromCanonicalName(const QString &strName,
	KKeyboardShortcut *pShortcut);
bool IsSecureAttentionShortcut(const KKeyboardShortcut &shortcut);

Q_DECLARE_OPERATORS_FOR_FLAGS(KKeyboardShortcutModifiers)
Q_DECLARE_METATYPE(KKeyboardShortcut)

#endif // _WINREMOTECONTROL_CORE_INPUT_KEYBOARDSHORTCUT_H_
