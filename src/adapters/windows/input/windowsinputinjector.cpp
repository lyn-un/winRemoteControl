#include "adapters/windows/input/windowsinputinjector.h"

#include <Windows.h>

namespace
{
	constexpr quint32 kExtendedKeyMask = 0x10000;
}

KWindowsInputInjector::~KWindowsInputInjector()
{
	QStringList errorMessages;
	releaseAllInputs(&errorMessages);
}

bool KWindowsInputInjector::inject(const KInputMessage &message, QString *pErrorMessage)
{
	if (message.type == MouseMoveInputMessageType)
		return sendMouseMove(message.nX, message.nY, pErrorMessage);
	if (message.type == MouseButtonInputMessageType)
	{
		return sendMouseButton(message.nX,
			message.nY,
			message.mouseButton,
			message.bPressed,
			pErrorMessage);
	}
	if (message.type == MouseWheelInputMessageType)
		return sendMouseWheel(message.nX, message.nY, message.nWheelDelta, pErrorMessage);
	if (message.type == KeyInputMessageType)
	{
		return sendKey(message.nVirtualKey,
			message.bPressed,
			message.bExtended,
			pErrorMessage);
	}

	if (pErrorMessage != nullptr)
		*pErrorMessage = QStringLiteral("Invalid remote input message type");
	return false;
}

void KWindowsInputInjector::releaseAllKeys(QStringList *pErrorMessages)
{
	const QSet<quint32> pressedKeys = m_pressedKeys;
	for (const quint32 nKeyId : pressedKeys)
	{
		QString strError;
		if (!sendKey(static_cast<int>(nKeyId & 0xFF),
				false,
				(nKeyId & kExtendedKeyMask) != 0,
				&strError)
			&& pErrorMessages != nullptr
			&& !strError.isEmpty())
		{
			pErrorMessages->append(strError);
		}
	}
	m_pressedKeys.clear();
}

void KWindowsInputInjector::releaseAllInputs(QStringList *pErrorMessages)
{
	releaseAllKeys(pErrorMessages);
	releaseAllMouseButtons(pErrorMessages);
}

bool KWindowsInputInjector::sendMouseMove(int nX, int nY, QString *pErrorMessage)
{
	const int nScreenWidth = GetSystemMetrics(SM_CXSCREEN);
	const int nScreenHeight = GetSystemMetrics(SM_CYSCREEN);
	if (nScreenWidth <= 1 || nScreenHeight <= 1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid primary screen size");
		return false;
	}

	const int nClampedX = clampToRange(nX, 0, nScreenWidth - 1);
	const int nClampedY = clampToRange(nY, 0, nScreenHeight - 1);
	INPUT input = {};
	input.type = INPUT_MOUSE;
	input.mi.dx = static_cast<LONG>((static_cast<long long>(nClampedX) * 65535) / (nScreenWidth - 1));
	input.mi.dy = static_cast<LONG>((static_cast<long long>(nClampedY) * 65535) / (nScreenHeight - 1));
	input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

	if (SendInput(1, &input, sizeof(INPUT)) != 1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = lastWin32ErrorMessage(QStringLiteral("Send mouse move failed"));
		return false;
	}
	return true;
}

bool KWindowsInputInjector::sendMouseButton(int nX,
	int nY,
	KRemoteMouseButton button,
	bool bPressed,
	QString *pErrorMessage)
{
	if (!sendMouseMove(nX, nY, pErrorMessage))
		return false;

	DWORD dwFlags = 0;
	if (button == LeftRemoteMouseButton)
		dwFlags = bPressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
	else if (button == RightRemoteMouseButton)
		dwFlags = bPressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
	else
		return false;

	INPUT input = {};
	input.type = INPUT_MOUSE;
	input.mi.dwFlags = dwFlags;
	if (SendInput(1, &input, sizeof(INPUT)) != 1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = lastWin32ErrorMessage(QStringLiteral("Send mouse button failed"));
		return false;
	}

	if (bPressed)
		m_pressedMouseButtons.insert(static_cast<int>(button));
	else
		m_pressedMouseButtons.remove(static_cast<int>(button));
	return true;
}

bool KWindowsInputInjector::sendMouseWheel(int nX,
	int nY,
	int nDelta,
	QString *pErrorMessage)
{
	if (!sendMouseMove(nX, nY, pErrorMessage))
		return false;

	INPUT input = {};
	input.type = INPUT_MOUSE;
	input.mi.mouseData = static_cast<DWORD>(nDelta);
	input.mi.dwFlags = MOUSEEVENTF_WHEEL;
	if (SendInput(1, &input, sizeof(INPUT)) != 1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = lastWin32ErrorMessage(QStringLiteral("Send mouse wheel failed"));
		return false;
	}
	return true;
}

bool KWindowsInputInjector::sendKey(int nVirtualKey,
	bool bPressed,
	bool bExtended,
	QString *pErrorMessage)
{
	INPUT input = {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = static_cast<WORD>(nVirtualKey);
	input.ki.dwFlags = (bPressed ? 0 : KEYEVENTF_KEYUP)
		| (bExtended ? KEYEVENTF_EXTENDEDKEY : 0);
	if (SendInput(1, &input, sizeof(INPUT)) != 1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = lastWin32ErrorMessage(QStringLiteral("Send keyboard input failed"));
		return false;
	}

	const quint32 nKeyId = static_cast<quint32>(nVirtualKey)
		| (bExtended ? kExtendedKeyMask : 0);
	if (bPressed)
		m_pressedKeys.insert(nKeyId);
	else
		m_pressedKeys.remove(nKeyId);
	return true;
}

void KWindowsInputInjector::releaseAllMouseButtons(QStringList *pErrorMessages)
{
	const QSet<int> pressedButtons = m_pressedMouseButtons;
	m_pressedMouseButtons.clear();
	for (const int nButton : pressedButtons)
	{
		DWORD dwFlags = 0;
		if (nButton == static_cast<int>(LeftRemoteMouseButton))
			dwFlags = MOUSEEVENTF_LEFTUP;
		else if (nButton == static_cast<int>(RightRemoteMouseButton))
			dwFlags = MOUSEEVENTF_RIGHTUP;
		else
			continue;

		INPUT input = {};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = dwFlags;
		if (SendInput(1, &input, sizeof(INPUT)) != 1 && pErrorMessages != nullptr)
		{
			pErrorMessages->append(
				lastWin32ErrorMessage(QStringLiteral("Release mouse button failed")));
		}
	}
}

int KWindowsInputInjector::clampToRange(int nValue, int nMinValue, int nMaxValue)
{
	if (nValue < nMinValue)
		return nMinValue;
	if (nValue > nMaxValue)
		return nMaxValue;
	return nValue;
}

QString KWindowsInputInjector::lastWin32ErrorMessage(const QString &strPrefix)
{
	return QStringLiteral("%1: Win32 error %2")
		.arg(strPrefix)
		.arg(static_cast<unsigned long>(GetLastError()));
}
