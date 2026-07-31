#include "input/inputinjector.h"

#include "common/latencytracelogger.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <Windows.h>

namespace
{
	constexpr char kType[] = "type";
	constexpr char kMouseMove[] = "mouseMove";
	constexpr char kMouseButton[] = "mouseButton";
	constexpr char kMouseWheel[] = "mouseWheel";
	constexpr char kKey[] = "key";
	constexpr char kButton[] = "button";
	constexpr char kLeft[] = "left";
	constexpr char kRight[] = "right";
	constexpr char kPressed[] = "pressed";
	constexpr char kX[] = "x";
	constexpr char kY[] = "y";
	constexpr char kDelta[] = "delta";
	constexpr char kSeq[] = "seq";
	constexpr char kTrace[] = "trace";
	constexpr char kVirtualKey[] = "vk";
	constexpr char kExtended[] = "extended";
	constexpr quint32 kExtendedKeyMask = 0x10000;

	static QString inputTraceExtra(const QJsonObject &object)
	{
		if (object.value(QString::fromLatin1(kType)).toString()
			== QString::fromLatin1(kKey))
		{
			return QStringLiteral("seq=%1 type=%2 pressed=%3")
				.arg(object.value(QString::fromLatin1(kSeq)).toString())
				.arg(object.value(QString::fromLatin1(kType)).toString())
				.arg(object.value(QString::fromLatin1(kPressed)).toBool() ? 1 : 0);
		}

		return QStringLiteral("seq=%1 type=%2 x=%3 y=%4")
			.arg(object.value(QString::fromLatin1(kSeq)).toString())
			.arg(object.value(QString::fromLatin1(kType)).toString())
			.arg(object.value(QString::fromLatin1(kX)).toInt())
			.arg(object.value(QString::fromLatin1(kY)).toInt());
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

void KInputInjector::handleInputMessage(const QString &strMessage)
{
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return;

	const QJsonObject object = document.object();
	const QString strType = object.value(QString::fromLatin1(kType)).toString();
	const int nX = object.value(QString::fromLatin1(kX)).toInt();
	const int nY = object.value(QString::fromLatin1(kY)).toInt();
	const bool bTrace = object.value(QString::fromLatin1(kTrace)).toBool(false)
		|| (strType == QString::fromLatin1(kKey) && KLatencyTraceLogger::isEnabled());

	QString strError;
	bool bOk = true;
	QElapsedTimer timer;
	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("inject_begin"),
			inputTraceExtra(object));
		timer.start();
	}

	if (strType == QString::fromLatin1(kMouseMove))
	{
		bOk = sendMouseMove(nX, nY, &strError);
	}
	else if (strType == QString::fromLatin1(kMouseButton))
	{
		bOk = sendMouseButton(nX,
			nY,
			object.value(QString::fromLatin1(kButton)).toString(),
			object.value(QString::fromLatin1(kPressed)).toBool(),
			&strError);
	}
	else if (strType == QString::fromLatin1(kMouseWheel))
	{
		bOk = sendMouseWheel(nX,
			nY,
			object.value(QString::fromLatin1(kDelta)).toInt(),
			&strError);
	}
	else if (strType == QString::fromLatin1(kKey))
	{
		const QJsonValue virtualKeyValue = object.value(QString::fromLatin1(kVirtualKey));
		const QJsonValue pressedValue = object.value(QString::fromLatin1(kPressed));
		const QJsonValue extendedValue = object.value(QString::fromLatin1(kExtended));
		const int nVirtualKey = virtualKeyValue.toInt();
		if (!virtualKeyValue.isDouble()
			|| !pressedValue.isBool()
			|| (!extendedValue.isUndefined() && !extendedValue.isBool())
			|| nVirtualKey <= 0
			|| nVirtualKey > 0xFF)
		{
			bOk = false;
			strError = QStringLiteral("Invalid remote key message");
		}
		else
		{
			bOk = sendKey(nVirtualKey,
				pressedValue.toBool(),
				extendedValue.toBool(false),
				&strError);
		}
	}

	if (!bOk && !strError.isEmpty())
		emit inputError(strError);

	const quint64 nSeq = object.value(QString::fromLatin1(kSeq)).toString().toULongLong();
	if (bOk && nSeq > 0)
		emit inputInjected(nSeq, QDateTime::currentMSecsSinceEpoch());

	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("inject_end"),
			QStringLiteral("%1 costMs=%2 ok=%3")
				.arg(inputTraceExtra(object))
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
	const QString &strButton,
	bool bPressed,
	QString *pErrorMessage)
{
	if (!sendMouseMove(nX, nY, pErrorMessage))
		return false;

	DWORD dwFlags = 0;
	if (strButton == QString::fromLatin1(kLeft))
		dwFlags = bPressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
	else if (strButton == QString::fromLatin1(kRight))
		dwFlags = bPressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
	else
		return true;

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
		m_pressedMouseButtons.insert(strButton);
	else
		m_pressedMouseButtons.remove(strButton);

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
	const QSet<QString> pressedButtons = m_pressedMouseButtons;
	m_pressedMouseButtons.clear();
	for (const QString &strButton : pressedButtons)
	{
		DWORD dwFlags = 0;
		if (strButton == QString::fromLatin1(kLeft))
			dwFlags = MOUSEEVENTF_LEFTUP;
		else if (strButton == QString::fromLatin1(kRight))
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
