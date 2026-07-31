#include "input/inputinjector.h"

#include "common/latencytracelogger.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>

#include <Windows.h>

namespace
{
	constexpr quint32 kExtendedKeyMask = 0x10000;

	static QString inputTraceExtra(const KInputMessage &message)
	{
		if (message.type == KeyInputMessageType)
		{
			return QStringLiteral("seq=%1 type=%2 pressed=%3")
				.arg(message.nSequence)
				.arg(KInputMessageCodec::typeName(message.type))
				.arg(message.bPressed ? 1 : 0);
		}

		return QStringLiteral("seq=%1 type=%2 x=%3 y=%4")
			.arg(message.nSequence)
			.arg(KInputMessageCodec::typeName(message.type))
			.arg(message.nX)
			.arg(message.nY);
	}
}

KInputInjector::KInputInjector(QObject *pParent)
	: QObject(pParent)
{
}

KInputInjector::~KInputInjector()
{
	releaseAllInputs();
}

void KInputInjector::handleInputMessage(const KInputMessage &message)
{
	const bool bTrace = message.bTrace
		|| (message.type == KeyInputMessageType && KLatencyTraceLogger::isEnabled());

	QString strError;
	bool bOk = true;
	QElapsedTimer timer;
	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("inject_begin"),
			inputTraceExtra(message));
		timer.start();
	}

	if (message.type == MouseMoveInputMessageType)
	{
		bOk = sendMouseMove(message.nX, message.nY, &strError);
	}
	else if (message.type == MouseButtonInputMessageType)
	{
		bOk = sendMouseButton(message.nX,
			message.nY,
			message.mouseButton,
			message.bPressed,
			&strError);
	}
	else if (message.type == MouseWheelInputMessageType)
	{
		bOk = sendMouseWheel(message.nX,
			message.nY,
			message.nWheelDelta,
			&strError);
	}
	else if (message.type == KeyInputMessageType)
	{
		bOk = sendKey(message.nVirtualKey,
			message.bPressed,
			message.bExtended,
			&strError);
	}
	else
	{
		bOk = false;
		strError = QStringLiteral("Invalid remote input message type");
	}

	if (!bOk && !strError.isEmpty())
		emit inputError(strError);

	if (bOk && message.nSequence > 0)
		emit inputInjected(message.nSequence, QDateTime::currentMSecsSinceEpoch());

	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("inject_end"),
			QStringLiteral("%1 costMs=%2 ok=%3")
				.arg(inputTraceExtra(message))
				.arg(timer.isValid() ? timer.elapsed() : -1)
				.arg(bOk ? 1 : 0));
	}
}

void KInputInjector::releaseAllKeys()
{
	const QSet<quint32> pressedKeys = m_pressedKeys;
	for (const quint32 nKeyId : pressedKeys)
	{
		QString strError;
		if (!sendKey(static_cast<int>(nKeyId & 0xFF),
				false,
				(nKeyId & kExtendedKeyMask) != 0,
				&strError)
			&& !strError.isEmpty())
		{
			emit inputError(strError);
		}
	}
	m_pressedKeys.clear();
}

void KInputInjector::releaseAllInputs()
{
	releaseAllKeys();
	releaseAllMouseButtons();
}

bool KInputInjector::sendMouseMove(int nX, int nY, QString *pErrorMessage)
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

bool KInputInjector::sendMouseButton(int nX,
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

bool KInputInjector::sendMouseWheel(int nX, int nY, int nDelta, QString *pErrorMessage)
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

bool KInputInjector::sendKey(int nVirtualKey,
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

void KInputInjector::releaseAllMouseButtons()
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
		if (SendInput(1, &input, sizeof(INPUT)) != 1)
			emit inputError(lastWin32ErrorMessage(QStringLiteral("Release mouse button failed")));
	}
}

int KInputInjector::clampToRange(int nValue, int nMinValue, int nMaxValue)
{
	if (nValue < nMinValue)
		return nMinValue;
	if (nValue > nMaxValue)
		return nMaxValue;
	return nValue;
}

QString KInputInjector::lastWin32ErrorMessage(const QString &strPrefix)
{
	return QStringLiteral("%1: Win32 error %2")
		.arg(strPrefix)
		.arg(static_cast<unsigned long>(GetLastError()));
}
