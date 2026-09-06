#include "adapters/windows/input/windowsremotekeyboardcaptureadapter.h"

#include <QtCore/QTimer>

#include <array>

namespace
{
	constexpr ULONG_PTR kKeyboardReplayMarker = static_cast<ULONG_PTR>(0x5752434B45595250ULL);

	WORD ScanCodeForVirtualKey(quint32 nVirtualKey)
	{
		return static_cast<WORD>(MapVirtualKeyW(nVirtualKey, MAPVK_VK_TO_VSC_EX) & 0xFF);
	}

	bool IsExtendedVirtualKey(quint32 nVirtualKey)
	{
		switch (nVirtualKey)
		{
		case VK_RCONTROL:
		case VK_RMENU:
		case VK_INSERT:
		case VK_DELETE:
		case VK_HOME:
		case VK_END:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
		case VK_LWIN:
		case VK_RWIN:
			return true;
		default:
			return false;
		}
	}

	INPUT KeyboardInput(quint32 nVirtualKey, bool bPressed)
	{
		INPUT input = {};
		input.type = INPUT_KEYBOARD;
		input.ki.wVk = static_cast<WORD>(nVirtualKey);
		input.ki.wScan = ScanCodeForVirtualKey(nVirtualKey);
		input.ki.dwFlags = bPressed ? 0 : KEYEVENTF_KEYUP;
		if (IsExtendedVirtualKey(nVirtualKey))
			input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
		input.ki.dwExtraInfo = kKeyboardReplayMarker;
		return input;
	}
}

KWindowsRemoteKeyboardCaptureAdapter *
	KWindowsRemoteKeyboardCaptureAdapter::s_pActiveAdapter = nullptr;

KWindowsRemoteKeyboardCaptureAdapter::KWindowsRemoteKeyboardCaptureAdapter(QObject *pParent)
	: QObject(pParent)
{
}

KWindowsRemoteKeyboardCaptureAdapter::~KWindowsRemoteKeyboardCaptureAdapter()
{
	stop();
}

bool KWindowsRemoteKeyboardCaptureAdapter::start(EventHandler handler, QString *pError)
{
	if (m_hKeyboardHook != nullptr)
	{
		m_eventHandler = std::move(handler);
		return true;
	}
	if (s_pActiveAdapter != nullptr && s_pActiveAdapter != this)
	{
		if (pError != nullptr)
			*pError = QStringLiteral("键盘捕获已经被占用");
		return false;
	}
	m_eventHandler = std::move(handler);
	s_pActiveAdapter = this;
	m_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL,
		&KWindowsRemoteKeyboardCaptureAdapter::keyboardHookProcedure,
		GetModuleHandleW(nullptr), 0);
	if (m_hKeyboardHook != nullptr)
		return true;
	s_pActiveAdapter = nullptr;
	m_eventHandler = EventHandler();
	if (pError != nullptr)
		*pError = QStringLiteral("安装键盘捕获失败：%1").arg(GetLastError());
	return false;
}

void KWindowsRemoteKeyboardCaptureAdapter::stop()
{
	if (m_hKeyboardHook != nullptr)
		UnhookWindowsHookEx(m_hKeyboardHook);
	m_hKeyboardHook = nullptr;
	m_eventHandler = EventHandler();
	if (s_pActiveAdapter == this)
		s_pActiveAdapter = nullptr;
}

bool KWindowsRemoteKeyboardCaptureAdapter::isActive() const
{
	return m_hKeyboardHook != nullptr;
}

void KWindowsRemoteKeyboardCaptureAdapter::replayLocalShortcut(
	const KKeyboardShortcut &shortcut)
{
	if (!shortcut.isValid())
		return;
	QVector<quint32> keys;
	if (shortcut.modifiers.testFlag(ControlKeyboardShortcutModifier))
		keys.append(VK_CONTROL);
	if (shortcut.modifiers.testFlag(AltKeyboardShortcutModifier))
		keys.append(VK_MENU);
	if (shortcut.modifiers.testFlag(ShiftKeyboardShortcutModifier))
		keys.append(VK_SHIFT);
	if (shortcut.modifiers.testFlag(WindowsKeyboardShortcutModifier))
		keys.append(VK_LWIN);
	keys.append(shortcut.nVirtualKey);

	QVector<INPUT> inputs;
	inputs.reserve(keys.size() * 2);
	for (quint32 nVirtualKey : keys)
		inputs.append(KeyboardInput(nVirtualKey, true));
	for (auto iterator = keys.crbegin(); iterator != keys.crend(); ++iterator)
		inputs.append(KeyboardInput(*iterator, false));
	SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

quintptr KWindowsRemoteKeyboardCaptureAdapter::replayMarker()
{
	return static_cast<quintptr>(kKeyboardReplayMarker);
}

LRESULT CALLBACK KWindowsRemoteKeyboardCaptureAdapter::keyboardHookProcedure(
	int nCode,
	WPARAM wParam,
	LPARAM lParam)
{
	if (nCode != HC_ACTION || s_pActiveAdapter == nullptr)
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	const bool bPressed = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
	const bool bReleased = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
	if (!bPressed && !bReleased)
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	const auto *pKeyboard = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
	if (pKeyboard == nullptr)
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	if ((pKeyboard->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0)
		return CallNextHookEx(nullptr, nCode, wParam, lParam);
	KWindowsKeyboardEvent event;
	event.nVirtualKey = pKeyboard->vkCode;
	event.nScanCode = pKeyboard->scanCode;
	event.nFlags = pKeyboard->flags;
	event.bPressed = bPressed;
	if (s_pActiveAdapter->m_eventHandler
		&& s_pActiveAdapter->m_eventHandler(event))
	{
		return 1;
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
