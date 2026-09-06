#include "core/input/keyboardshortcut.h"

#include <QtCore/QStringList>

namespace
{
	constexpr quint32 kVirtualKeyBack = 0x08;
	constexpr quint32 kVirtualKeyTab = 0x09;
	constexpr quint32 kVirtualKeyReturn = 0x0D;
	constexpr quint32 kVirtualKeyEscape = 0x1B;
	constexpr quint32 kVirtualKeySpace = 0x20;
	constexpr quint32 kVirtualKeyPageUp = 0x21;
	constexpr quint32 kVirtualKeyPageDown = 0x22;
	constexpr quint32 kVirtualKeyEnd = 0x23;
	constexpr quint32 kVirtualKeyHome = 0x24;
	constexpr quint32 kVirtualKeyLeft = 0x25;
	constexpr quint32 kVirtualKeyUp = 0x26;
	constexpr quint32 kVirtualKeyRight = 0x27;
	constexpr quint32 kVirtualKeyDown = 0x28;
	constexpr quint32 kVirtualKeyDelete = 0x2E;
	constexpr quint32 kVirtualKey0 = 0x30;
	constexpr quint32 kVirtualKey9 = 0x39;
	constexpr quint32 kVirtualKeyA = 0x41;
	constexpr quint32 kVirtualKeyZ = 0x5A;
	constexpr quint32 kVirtualKeyLeftWindows = 0x5B;
	constexpr quint32 kVirtualKeyRightWindows = 0x5C;
	constexpr quint32 kVirtualKeyShift = 0x10;
	constexpr quint32 kVirtualKeyControl = 0x11;
	constexpr quint32 kVirtualKeyAlt = 0x12;
	constexpr quint32 kVirtualKeyF1 = 0x70;
	constexpr quint32 kVirtualKeyF24 = 0x87;
	constexpr quint32 kVirtualKeyLeftShift = 0xA0;
	constexpr quint32 kVirtualKeyRightShift = 0xA1;
	constexpr quint32 kVirtualKeyLeftControl = 0xA2;
	constexpr quint32 kVirtualKeyRightControl = 0xA3;
	constexpr quint32 kVirtualKeyLeftAlt = 0xA4;
	constexpr quint32 kVirtualKeyRightAlt = 0xA5;

	QString KeyDisplayName(quint32 nVirtualKey)
	{
		if ((nVirtualKey >= kVirtualKey0 && nVirtualKey <= kVirtualKey9)
			|| (nVirtualKey >= kVirtualKeyA && nVirtualKey <= kVirtualKeyZ))
		{
			return QString(QChar(static_cast<ushort>(nVirtualKey)));
		}
		if (nVirtualKey >= kVirtualKeyF1 && nVirtualKey <= kVirtualKeyF24)
			return QStringLiteral("F%1").arg(nVirtualKey - kVirtualKeyF1 + 1);
		switch (nVirtualKey)
		{
		case kVirtualKeyBack: return QStringLiteral("Backspace");
		case kVirtualKeyTab: return QStringLiteral("Tab");
		case kVirtualKeyReturn: return QStringLiteral("Enter");
		case kVirtualKeyEscape: return QStringLiteral("Escape");
		case kVirtualKeySpace: return QStringLiteral("Space");
		case kVirtualKeyPageUp: return QStringLiteral("PageUp");
		case kVirtualKeyPageDown: return QStringLiteral("PageDown");
		case kVirtualKeyEnd: return QStringLiteral("End");
		case kVirtualKeyHome: return QStringLiteral("Home");
		case kVirtualKeyLeft: return QStringLiteral("Left");
		case kVirtualKeyUp: return QStringLiteral("Up");
		case kVirtualKeyRight: return QStringLiteral("Right");
		case kVirtualKeyDown: return QStringLiteral("Down");
		case kVirtualKeyDelete: return QStringLiteral("Delete");
		default:
			return QStringLiteral("VK_%1")
				.arg(nVirtualKey, 2, 16, QLatin1Char('0')).toUpper();
		}
	}

	bool KeyFromName(const QString &strName, quint32 *pVirtualKey)
	{
		if (pVirtualKey == nullptr || strName.isEmpty())
			return false;
		const QString strUpper = strName.toUpper();
		if (strUpper.size() == 1)
		{
			const ushort nValue = strUpper.at(0).unicode();
			if ((nValue >= kVirtualKey0 && nValue <= kVirtualKey9)
				|| (nValue >= kVirtualKeyA && nValue <= kVirtualKeyZ))
			{
				*pVirtualKey = nValue;
				return true;
			}
		}
		if (strUpper.startsWith(QStringLiteral("F")))
		{
			bool bOk = false;
			const int nFunction = strUpper.mid(1).toInt(&bOk);
			if (bOk && nFunction >= 1 && nFunction <= 24)
			{
				*pVirtualKey = kVirtualKeyF1 + static_cast<quint32>(nFunction - 1);
				return true;
			}
		}
		const struct { const char *pName; quint32 nVirtualKey; } names[] = {
			{ "BACKSPACE", kVirtualKeyBack }, { "TAB", kVirtualKeyTab },
			{ "ENTER", kVirtualKeyReturn }, { "ESCAPE", kVirtualKeyEscape },
			{ "SPACE", kVirtualKeySpace }, { "PAGEUP", kVirtualKeyPageUp },
			{ "PAGEDOWN", kVirtualKeyPageDown }, { "END", kVirtualKeyEnd },
			{ "HOME", kVirtualKeyHome }, { "LEFT", kVirtualKeyLeft },
			{ "UP", kVirtualKeyUp }, { "RIGHT", kVirtualKeyRight },
			{ "DOWN", kVirtualKeyDown }, { "DELETE", kVirtualKeyDelete }
		};
		for (const auto &name : names)
		{
			if (strUpper == QString::fromLatin1(name.pName))
			{
				*pVirtualKey = name.nVirtualKey;
				return true;
			}
		}
		if (strUpper.startsWith(QStringLiteral("VK_")))
		{
			bool bOk = false;
			const uint nValue = strUpper.mid(3).toUInt(&bOk, 16);
			if (bOk && nValue > 0 && nValue <= 0xFF)
			{
				*pVirtualKey = nValue;
				return true;
			}
		}
		return false;
	}
}

bool KKeyboardShortcut::isValid() const
{
	return nVirtualKey > 0 && nVirtualKey <= 0xFF
		&& ModifierForVirtualKey(nVirtualKey) == NoKeyboardShortcutModifier;
}

bool KKeyboardShortcut::operator==(const KKeyboardShortcut &other) const
{
	return modifiers == other.modifiers && nVirtualKey == other.nVirtualKey;
}

KKeyboardShortcut DefaultReleaseKeyboardCaptureShortcut()
{
	return { ControlKeyboardShortcutModifier | AltKeyboardShortcutModifier, 0x0D };
}

KKeyboardShortcut DefaultToggleFullscreenShortcut()
{
	return { ControlKeyboardShortcutModifier | AltKeyboardShortcutModifier, 'F' };
}

quint32 NormalizeShortcutVirtualKey(quint32 nVirtualKey)
{
	if (nVirtualKey == kVirtualKeyShift)
		return kVirtualKeyLeftShift;
	if (nVirtualKey == kVirtualKeyControl)
		return kVirtualKeyLeftControl;
	if (nVirtualKey == kVirtualKeyAlt)
		return kVirtualKeyLeftAlt;
	if (nVirtualKey == kVirtualKeyRightShift)
		return kVirtualKeyLeftShift;
	if (nVirtualKey == kVirtualKeyRightControl)
		return kVirtualKeyLeftControl;
	if (nVirtualKey == kVirtualKeyRightAlt)
		return kVirtualKeyLeftAlt;
	if (nVirtualKey == kVirtualKeyRightWindows)
		return kVirtualKeyLeftWindows;
	return nVirtualKey;
}

KKeyboardShortcutModifier ModifierForVirtualKey(quint32 nVirtualKey)
{
	switch (NormalizeShortcutVirtualKey(nVirtualKey))
	{
	case kVirtualKeyLeftControl: return ControlKeyboardShortcutModifier;
	case kVirtualKeyLeftAlt: return AltKeyboardShortcutModifier;
	case kVirtualKeyLeftShift: return ShiftKeyboardShortcutModifier;
	case kVirtualKeyLeftWindows: return WindowsKeyboardShortcutModifier;
	default: return NoKeyboardShortcutModifier;
	}
}

QString KeyboardShortcutCanonicalName(const KKeyboardShortcut &shortcut)
{
	if (!shortcut.isValid())
		return QString();
	QStringList parts;
	if (shortcut.modifiers.testFlag(ControlKeyboardShortcutModifier))
		parts.append(QStringLiteral("Ctrl"));
	if (shortcut.modifiers.testFlag(AltKeyboardShortcutModifier))
		parts.append(QStringLiteral("Alt"));
	if (shortcut.modifiers.testFlag(ShiftKeyboardShortcutModifier))
		parts.append(QStringLiteral("Shift"));
	if (shortcut.modifiers.testFlag(WindowsKeyboardShortcutModifier))
		parts.append(QStringLiteral("Win"));
	parts.append(KeyDisplayName(shortcut.nVirtualKey));
	return parts.join(QLatin1Char('+'));
}

QString KeyboardShortcutDisplayName(const KKeyboardShortcut &shortcut)
{
	return KeyboardShortcutCanonicalName(shortcut).replace(QLatin1Char('+'), QStringLiteral(" + "));
}

bool KeyboardShortcutFromCanonicalName(const QString &strName,
	KKeyboardShortcut *pShortcut)
{
	if (pShortcut == nullptr)
		return false;
	KKeyboardShortcut shortcut;
	const QStringList parts = strName.split(QLatin1Char('+'), Qt::SkipEmptyParts);
	if (parts.isEmpty())
		return false;
	for (int nIndex = 0; nIndex < parts.size() - 1; ++nIndex)
	{
		const QString strPart = parts.at(nIndex).trimmed().toUpper();
		if (strPart == QStringLiteral("CTRL"))
			shortcut.modifiers |= ControlKeyboardShortcutModifier;
		else if (strPart == QStringLiteral("ALT"))
			shortcut.modifiers |= AltKeyboardShortcutModifier;
		else if (strPart == QStringLiteral("SHIFT"))
			shortcut.modifiers |= ShiftKeyboardShortcutModifier;
		else if (strPart == QStringLiteral("WIN"))
			shortcut.modifiers |= WindowsKeyboardShortcutModifier;
		else
			return false;
	}
	if (!KeyFromName(parts.constLast().trimmed(), &shortcut.nVirtualKey)
		|| !shortcut.isValid())
	{
		return false;
	}
	*pShortcut = shortcut;
	return true;
}

bool IsSecureAttentionShortcut(const KKeyboardShortcut &shortcut)
{
	return shortcut.nVirtualKey == 0x2E
		&& shortcut.modifiers.testFlag(ControlKeyboardShortcutModifier)
		&& shortcut.modifiers.testFlag(AltKeyboardShortcutModifier);
}
