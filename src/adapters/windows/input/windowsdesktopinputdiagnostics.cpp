#include "adapters/windows/input/windowsdesktopinputdiagnostics.h"

#include "common/desktopinputtracelogger.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <TlHelp32.h>

#include <QtCore/QByteArray>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>

#include <atomic>
#include <chrono>
#include <iterator>

namespace
{
constexpr int kMaximumWindowChainDepth = 8;
constexpr UINT kWindowQueryTimeoutMs = 50;
constexpr DWORD kMouseMoveTraceIntervalMs = 25;

std::atomic<quint64> g_nLastButtonEventId = 0;
std::atomic_bool g_bMouseObserverActive = false;
std::atomic_uint g_nObservedPressedButtons = 0;
std::atomic<DWORD> g_nLastMouseMoveTraceTime = 0;
std::atomic<quint64> g_nNextHookSequence = 1;

QString HandleToken(HWND hWindow)
{
	return hWindow == nullptr
		? QStringLiteral("0")
		: QStringLiteral("0x%1").arg(
			QString::number(reinterpret_cast<quintptr>(hWindow), 16));
}

QString WindowClassName(HWND hWindow)
{
	wchar_t className[256] = {};
	const int nCharacters = GetClassNameW(
		hWindow, className, static_cast<int>(std::size(className)));
	return nCharacters > 0
		? QString::fromWCharArray(className, nCharacters)
		: QStringLiteral("unknown");
}

QString ProcessFileName(DWORD nProcessId)
{
	if (nProcessId == 0)
		return QStringLiteral("unknown");
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
		FALSE, nProcessId);
	if (hProcess == nullptr)
		return QStringLiteral("unknown");

	wchar_t processPath[32768] = {};
	DWORD nCharacters = static_cast<DWORD>(std::size(processPath));
	const BOOL bQueried = QueryFullProcessImageNameW(
		hProcess, 0, processPath, &nCharacters);
	CloseHandle(hProcess);
	return bQueried
		? KWindowsDesktopInputDiagnostics::processFileNameToken(
			QString::fromWCharArray(processPath, static_cast<int>(nCharacters)))
		: QStringLiteral("unknown");
}

QString ProcessSecurityToken(DWORD nProcessId)
{
	if (nProcessId == 0)
		return QStringLiteral("pid=0 integrity=unknown error=invalid_pid");
	HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, nProcessId);
	if (hProcess == nullptr)
	{
		return QStringLiteral("pid=%1 integrity=unknown error=open_process_%2")
			.arg(nProcessId)
			.arg(GetLastError());
	}

	HANDLE hToken = nullptr;
	if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken) == FALSE)
	{
		const DWORD nError = GetLastError();
		CloseHandle(hProcess);
		return QStringLiteral("pid=%1 integrity=unknown error=open_token_%2")
			.arg(nProcessId)
			.arg(nError);
	}

	DWORD nRequiredSize = 0;
	SetLastError(ERROR_SUCCESS);
	GetTokenInformation(hToken, TokenIntegrityLevel, nullptr, 0, &nRequiredSize);
	DWORD nIntegrityError = nRequiredSize > 0 ? ERROR_SUCCESS : GetLastError();
	QByteArray integrityBuffer(static_cast<int>(nRequiredSize), Qt::Uninitialized);
	DWORD nIntegrityRid = 0;
	bool bHasIntegrity = nRequiredSize > 0
		&& GetTokenInformation(hToken,
			TokenIntegrityLevel,
			integrityBuffer.data(),
			nRequiredSize,
			&nRequiredSize) != FALSE;
	if (!bHasIntegrity && nIntegrityError == ERROR_SUCCESS)
		nIntegrityError = GetLastError();
	if (bHasIntegrity)
	{
		const auto *pLabel = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(
			integrityBuffer.constData());
		const DWORD nSubAuthorityCount = *GetSidSubAuthorityCount(pLabel->Label.Sid);
		bHasIntegrity = nSubAuthorityCount > 0;
		if (bHasIntegrity)
			nIntegrityRid = *GetSidSubAuthority(pLabel->Label.Sid, nSubAuthorityCount - 1);
	}

	TOKEN_ELEVATION elevation = {};
	DWORD nElevationSize = sizeof(elevation);
	SetLastError(ERROR_SUCCESS);
	const bool bHasElevation = GetTokenInformation(hToken,
		TokenElevation,
		&elevation,
		sizeof(elevation),
		&nElevationSize) != FALSE;
	const DWORD nElevationError = bHasElevation ? ERROR_SUCCESS : GetLastError();
	DWORD nUiAccess = 0;
	DWORD nUiAccessSize = sizeof(nUiAccess);
	SetLastError(ERROR_SUCCESS);
	const bool bHasUiAccess = GetTokenInformation(hToken,
		TokenUIAccess,
		&nUiAccess,
		sizeof(nUiAccess),
		&nUiAccessSize) != FALSE;
	const DWORD nUiAccessError = bHasUiAccess ? ERROR_SUCCESS : GetLastError();

	CloseHandle(hToken);
	CloseHandle(hProcess);
	return QStringLiteral(
		"pid=%1 integrity=%2 integrityError=%3 elevated=%4 elevationError=%5 "
		"uiAccess=%6 uiAccessError=%7")
		.arg(nProcessId)
		.arg(bHasIntegrity
			? KWindowsDesktopInputDiagnostics::integrityLevelName(nIntegrityRid)
			: QStringLiteral("unknown"))
		.arg(nIntegrityError)
		.arg(bHasElevation ? QString::number(elevation.TokenIsElevated)
			: QStringLiteral("unknown"))
		.arg(nElevationError)
		.arg(bHasUiAccess ? QString::number(nUiAccess) : QStringLiteral("unknown"))
		.arg(nUiAccessError);
}

QString RunningN0vaSecurityToken()
{
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return QStringLiteral("none error=snapshot_%1").arg(GetLastError());

	QStringList processes;
	PROCESSENTRY32W processEntry = {};
	processEntry.dwSize = sizeof(processEntry);
	if (Process32FirstW(hSnapshot, &processEntry) != FALSE)
	{
		do
		{
			if (QString::fromWCharArray(processEntry.szExeFile).compare(
				QStringLiteral("N0vaDesktop.exe"), Qt::CaseInsensitive) == 0)
			{
				processes.append(ProcessSecurityToken(processEntry.th32ProcessID));
			}
		}
		while (Process32NextW(hSnapshot, &processEntry) != FALSE);
	}
	CloseHandle(hSnapshot);
	return processes.isEmpty() ? QStringLiteral("none") : processes.join(QLatin1Char('|'));
}

QString GuiThreadInfoToken(DWORD nThreadId)
{
	if (nThreadId == 0)
		return QStringLiteral("thread=0 error=invalid_thread");
	GUITHREADINFO info = {};
	info.cbSize = sizeof(info);
	SetLastError(ERROR_SUCCESS);
	if (GetGUIThreadInfo(nThreadId, &info) == FALSE)
	{
		return QStringLiteral("thread=%1 error=%2")
			.arg(nThreadId)
			.arg(GetLastError());
	}
	return QStringLiteral(
		"thread=%1 flags=0x%2 active=%3 focus=%4 capture=%5 menu=%6 moveSize=%7 caret=%8")
		.arg(nThreadId)
		.arg(QString::number(info.flags, 16))
		.arg(HandleToken(info.hwndActive))
		.arg(HandleToken(info.hwndFocus))
		.arg(HandleToken(info.hwndCapture))
		.arg(HandleToken(info.hwndMenuOwner))
		.arg(HandleToken(info.hwndMoveSize))
		.arg(HandleToken(info.hwndCaret));
}

QString WindowToken(HWND hWindow)
{
	if (hWindow == nullptr)
		return QStringLiteral("0:none");
	DWORD nProcessId = 0;
	GetWindowThreadProcessId(hWindow, &nProcessId);
	const QString strClassName = WindowClassName(hWindow);
	const QString strProcessFileName = ProcessFileName(nProcessId);
	const KDesktopInputWindowKind kind =
		KWindowsDesktopInputDiagnostics::classifyWindow(
			strClassName, strProcessFileName);
	const LONG_PTR nStyle = GetWindowLongPtrW(hWindow, GWL_STYLE);
	const LONG_PTR nExtendedStyle = GetWindowLongPtrW(hWindow, GWL_EXSTYLE);
	return QStringLiteral("%1:%2:%3:%4:%5:visible=%6:enabled=%7:style=0x%8:ex=0x%9")
		.arg(HandleToken(hWindow))
		.arg(strClassName.simplified().replace(QLatin1Char(' '), QLatin1Char('_')))
		.arg(nProcessId)
		.arg(strProcessFileName)
		.arg(static_cast<int>(kind))
		.arg(IsWindowVisible(hWindow) != FALSE ? 1 : 0)
		.arg(IsWindowEnabled(hWindow) != FALSE ? 1 : 0)
		.arg(QString::number(static_cast<quintptr>(nStyle), 16))
		.arg(QString::number(static_cast<quintptr>(nExtendedStyle), 16));
}

QString WindowChainToken(HWND hWindow)
{
	QStringList chain;
	HWND hCurrent = hWindow;
	for (int i = 0; hCurrent != nullptr && i < kMaximumWindowChainDepth; ++i)
	{
		chain.append(WindowToken(hCurrent));
		HWND hParent = GetParent(hCurrent);
		if (hParent == hCurrent)
			break;
		hCurrent = hParent;
	}
	return chain.isEmpty() ? QStringLiteral("none") : chain.join(QLatin1Char('>'));
}

bool IsExplorerDesktopListView(HWND hWindow)
{
	if (WindowClassName(hWindow).compare(
		QStringLiteral("SysListView32"), Qt::CaseInsensitive) != 0)
	{
		return false;
	}
	DWORD nProcessId = 0;
	GetWindowThreadProcessId(hWindow, &nProcessId);
	if (ProcessFileName(nProcessId).compare(
		QStringLiteral("explorer.exe"), Qt::CaseInsensitive) != 0)
	{
		return false;
	}

	QStringList ancestorClassNames;
	for (HWND hParent = GetParent(hWindow);
		hParent != nullptr; hParent = GetParent(hParent))
		ancestorClassNames.append(WindowClassName(hParent));
	return KWindowsDesktopInputDiagnostics::isDesktopListViewHierarchy(
		ProcessFileName(nProcessId), ancestorClassNames);
}

BOOL CALLBACK FindDesktopListViewChild(HWND hWindow, LPARAM nParameter)
{
	HWND *pResult = reinterpret_cast<HWND *>(nParameter);
	if (pResult == nullptr)
		return FALSE;
	if (IsExplorerDesktopListView(hWindow))
	{
		*pResult = hWindow;
		return FALSE;
	}
	return TRUE;
}

BOOL CALLBACK FindDesktopListViewTopLevel(HWND hWindow, LPARAM nParameter)
{
	HWND *pResult = reinterpret_cast<HWND *>(nParameter);
	if (pResult == nullptr)
		return FALSE;
	if (IsExplorerDesktopListView(hWindow))
	{
		*pResult = hWindow;
		return FALSE;
	}
	EnumChildWindows(hWindow, FindDesktopListViewChild, nParameter);
	return *pResult == nullptr;
}

HWND DesktopListView()
{
	HWND hResult = nullptr;
	EnumWindows(FindDesktopListViewTopLevel,
		reinterpret_cast<LPARAM>(&hResult));
	return hResult;
}

QString DesktopStateToken(HWND hDesktopListView, int nX, int nY)
{
	if (hDesktopListView == nullptr)
		return QStringLiteral("desktop=missing");

	RECT rect = {};
	const bool bHasRect = GetWindowRect(hDesktopListView, &rect) != FALSE;
	const bool bPointInside = bHasRect
		&& nX >= rect.left && nX < rect.right
		&& nY >= rect.top && nY < rect.bottom;
	DWORD nThreadId = GetWindowThreadProcessId(hDesktopListView, nullptr);
	GUITHREADINFO threadInfo = {};
	threadInfo.cbSize = sizeof(threadInfo);
	SetLastError(ERROR_SUCCESS);
	const bool bHasThreadInfo = nThreadId != 0
		&& GetGUIThreadInfo(nThreadId, &threadInfo) != FALSE;
	const DWORD nThreadInfoError = bHasThreadInfo ? ERROR_SUCCESS : GetLastError();
	DWORD_PTR nSelectedCount = 0;
	const bool bHasSelectedCount = SendMessageTimeoutW(hDesktopListView,
		LVM_GETSELECTEDCOUNT,
		0,
		0,
		SMTO_ABORTIFHUNG | SMTO_BLOCK,
		kWindowQueryTimeoutMs,
		&nSelectedCount) != 0;

	return QStringLiteral(
		"desktop=%1 rect=%2,%3,%4,%5 pointInside=%6 selected=%7 desktopGui={%8}")
		.arg(WindowToken(hDesktopListView))
		.arg(bHasRect ? rect.left : 0)
		.arg(bHasRect ? rect.top : 0)
		.arg(bHasRect ? rect.right : 0)
		.arg(bHasRect ? rect.bottom : 0)
		.arg(bPointInside ? 1 : 0)
		.arg(bHasSelectedCount ? QString::number(nSelectedCount)
			: QStringLiteral("unknown"))
		.arg(bHasThreadInfo ? GuiThreadInfoToken(nThreadId)
			: QStringLiteral("thread=%1 error=%2").arg(nThreadId).arg(nThreadInfoError));
}

QString PointSnapshot(int nX, int nY)
{
	const POINT desiredPoint = { nX, nY };
	POINT cursorPoint = {};
	const bool bHasCursor = GetCursorPos(&cursorPoint) != FALSE;
	const HWND hDesiredTarget = WindowFromPoint(desiredPoint);
	const HWND hCursorTarget = bHasCursor ? WindowFromPoint(cursorPoint) : nullptr;
	const HWND hDesktopListView = DesktopListView();
	DWORD nDesiredProcessId = 0;
	const DWORD nDesiredThreadId = GetWindowThreadProcessId(
		hDesiredTarget, &nDesiredProcessId);
	const HWND hForeground = GetForegroundWindow();
	const DWORD nForegroundThreadId = GetWindowThreadProcessId(hForeground, nullptr);
	return QStringLiteral(
		"desired=%1,%2 cursor=%3,%4 desiredChain=%5 cursorChain=%6 targetSecurity={%7} "
		"targetGui={%8} foreground=%9 foregroundGui={%10} %11")
		.arg(nX)
		.arg(nY)
		.arg(bHasCursor ? cursorPoint.x : 0)
		.arg(bHasCursor ? cursorPoint.y : 0)
		.arg(WindowChainToken(hDesiredTarget))
		.arg(WindowChainToken(hCursorTarget))
		.arg(ProcessSecurityToken(nDesiredProcessId))
		.arg(GuiThreadInfoToken(nDesiredThreadId))
		.arg(WindowToken(hForeground))
		.arg(GuiThreadInfoToken(nForegroundThreadId))
		.arg(DesktopStateToken(hDesktopListView, nX, nY));
}

QString LowLevelMouseMessageName(WPARAM nMessage)
{
	switch (nMessage)
	{
	case WM_MOUSEMOVE:
		return QStringLiteral("move");
	case WM_LBUTTONDOWN:
		return QStringLiteral("left_down");
	case WM_LBUTTONUP:
		return QStringLiteral("left_up");
	case WM_RBUTTONDOWN:
		return QStringLiteral("right_down");
	case WM_RBUTTONUP:
		return QStringLiteral("right_up");
	case WM_MBUTTONDOWN:
		return QStringLiteral("middle_down");
	case WM_MBUTTONUP:
		return QStringLiteral("middle_up");
	case WM_XBUTTONDOWN:
		return QStringLiteral("x_down");
	case WM_XBUTTONUP:
		return QStringLiteral("x_up");
	case WM_MOUSEWHEEL:
		return QStringLiteral("wheel");
	case WM_MOUSEHWHEEL:
		return QStringLiteral("horizontal_wheel");
	default:
		return QStringLiteral("other_%1").arg(static_cast<qulonglong>(nMessage));
	}
}

unsigned int MouseButtonMask(WPARAM nMessage)
{
	if (nMessage == WM_LBUTTONDOWN || nMessage == WM_LBUTTONUP)
		return 0x01;
	if (nMessage == WM_RBUTTONDOWN || nMessage == WM_RBUTTONUP)
		return 0x02;
	if (nMessage == WM_MBUTTONDOWN || nMessage == WM_MBUTTONUP)
		return 0x04;
	if (nMessage == WM_XBUTTONDOWN || nMessage == WM_XBUTTONUP)
		return 0x08;
	return 0;
}

bool IsMouseButtonDownMessage(WPARAM nMessage)
{
	return nMessage == WM_LBUTTONDOWN || nMessage == WM_RBUTTONDOWN
		|| nMessage == WM_MBUTTONDOWN || nMessage == WM_XBUTTONDOWN;
}

bool IsMouseButtonUpMessage(WPARAM nMessage)
{
	return nMessage == WM_LBUTTONUP || nMessage == WM_RBUTTONUP
		|| nMessage == WM_MBUTTONUP || nMessage == WM_XBUTTONUP;
}

LRESULT CALLBACK HandleLowLevelMouse(int nCode, WPARAM nMessage, LPARAM nParameter)
{
	if (nCode != HC_ACTION || nParameter == 0)
		return CallNextHookEx(nullptr, nCode, nMessage, nParameter);

	const auto *pMouse = reinterpret_cast<const MSLLHOOKSTRUCT *>(nParameter);
	const unsigned int nButtonMask = MouseButtonMask(nMessage);
	if (IsMouseButtonDownMessage(nMessage))
		g_nObservedPressedButtons.fetch_or(nButtonMask);
	else if (IsMouseButtonUpMessage(nMessage))
		g_nObservedPressedButtons.fetch_and(~nButtonMask);

	if (nMessage == WM_MOUSEMOVE)
	{
		if (g_nObservedPressedButtons.load() == 0)
			return CallNextHookEx(nullptr, nCode, nMessage, nParameter);
		const DWORD nPreviousTime = g_nLastMouseMoveTraceTime.load();
		if (pMouse->time - nPreviousTime < kMouseMoveTraceIntervalMs)
			return CallNextHookEx(nullptr, nCode, nMessage, nParameter);
		g_nLastMouseMoveTraceTime.store(pMouse->time);
	}

	const quint64 nHookSequence = g_nNextHookSequence.fetch_add(1);
	const quint64 nRelatedButton = g_nLastButtonEventId.load();
	const qint64 nStartedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	KDesktopInputTraceLogger::write(QStringLiteral("hook_enter"),
		QStringLiteral(
			"hookSequence=%1 relatedButton=%2 message=%3 x=%4 y=%5 mouseData=%6 time=%7 flags={%8}")
			.arg(nHookSequence)
			.arg(nRelatedButton)
			.arg(LowLevelMouseMessageName(nMessage))
			.arg(pMouse->pt.x)
			.arg(pMouse->pt.y)
			.arg(pMouse->mouseData)
			.arg(pMouse->time)
			.arg(KWindowsDesktopInputDiagnostics::mouseHookFlagsToken(pMouse->flags)));

	const LRESULT nNextResult = CallNextHookEx(nullptr, nCode, nMessage, nParameter);
	const qint64 nFinishedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	KDesktopInputTraceLogger::write(QStringLiteral("hook_exit"),
		QStringLiteral("hookSequence=%1 relatedButton=%2 nextResult=%3 elapsedUs=%4")
			.arg(nHookSequence)
			.arg(nRelatedButton)
			.arg(static_cast<qint64>(nNextResult))
			.arg(nFinishedUs - nStartedUs));
	return nNextResult;
}

QString WinEventName(DWORD nEvent)
{
	if (nEvent == EVENT_OBJECT_FOCUS)
		return QStringLiteral("focus");
	if (nEvent == EVENT_OBJECT_SELECTION)
		return QStringLiteral("selection");
	if (nEvent == EVENT_OBJECT_SELECTIONADD)
		return QStringLiteral("selection_add");
	if (nEvent == EVENT_OBJECT_SELECTIONREMOVE)
		return QStringLiteral("selection_remove");
	if (nEvent == EVENT_OBJECT_SELECTIONWITHIN)
		return QStringLiteral("selection_within");
	return QStringLiteral("unknown");
}

void CALLBACK HandleDesktopWinEvent(HWINEVENTHOOK,
	DWORD nEvent,
	HWND hWindow,
	LONG nObjectId,
	LONG nChildId,
	DWORD nEventThread,
	DWORD nEventTime)
{
	const HWND hDesktopListView = DesktopListView();
	if (hDesktopListView == nullptr
		|| (hWindow != hDesktopListView && !IsChild(hDesktopListView, hWindow)))
	{
		return;
	}
	KDesktopInputTraceLogger::write(QStringLiteral("explorer_event"),
		QStringLiteral(
			"relatedButton=%1 event=%2 hwnd=%3 object=%4 child=%5 eventThread=%6 eventTime=%7 %8")
			.arg(g_nLastButtonEventId.load())
			.arg(WinEventName(nEvent))
			.arg(HandleToken(hWindow))
			.arg(nObjectId)
			.arg(nChildId)
			.arg(nEventThread)
			.arg(nEventTime)
			.arg(DesktopStateToken(hDesktopListView, -1, -1)));
}
}

KWindowsDesktopInputDiagnostics::KWindowsDesktopInputDiagnostics()
	: m_bEnabled(KDesktopInputTraceLogger::isEnabled())
{
}

KWindowsDesktopInputDiagnostics::~KWindowsDesktopInputDiagnostics()
{
	writePendingDragLast();
	if (m_pMouseHook != nullptr)
	{
		UnhookWindowsHookEx(reinterpret_cast<HHOOK>(m_pMouseHook));
		g_bMouseObserverActive.store(false);
	}
	if (m_pWinEventHook != nullptr)
		UnhookWinEvent(reinterpret_cast<HWINEVENTHOOK>(m_pWinEventHook));
	if (m_bObserversStarted)
		KDesktopInputTraceLogger::write(QStringLiteral("observer_stop"));
}

quint64 KWindowsDesktopInputDiagnostics::beginMouseButton(int nX,
	int nY,
	int nButton,
	bool bPressed,
	bool bDuplicateState)
{
	if (!m_bEnabled)
		return 0;
	ensureObserversStarted();
	writePendingDragLast();
	m_bFirstDragMoveLogged = false;
	m_nButtonStateInterval = m_nNextEventId++;
	m_nLastButtonX = nX;
	m_nLastButtonY = nY;
	g_nLastButtonEventId.store(m_nButtonStateInterval);
	KDesktopInputTraceLogger::write(QStringLiteral("button_before"),
		QStringLiteral("event=%1 button=%2 pressed=%3 duplicate=%4 %5")
			.arg(m_nButtonStateInterval)
			.arg(nButton)
			.arg(bPressed ? 1 : 0)
			.arg(bDuplicateState ? 1 : 0)
			.arg(PointSnapshot(nX, nY)));
	return m_nButtonStateInterval;
}

void KWindowsDesktopInputDiagnostics::completeMouseButton(quint64 nEventId,
	unsigned int nInsertedInputs,
	unsigned long nWin32Error)
{
	if (!m_bEnabled || nEventId == 0)
		return;
	KDesktopInputTraceLogger::write(QStringLiteral("button_after"),
		QStringLiteral("event=%1 inserted=%2 error=%3 %4")
			.arg(nEventId)
			.arg(nInsertedInputs)
			.arg(nWin32Error)
			.arg(PointSnapshot(m_nLastButtonX, m_nLastButtonY)));
}

quint64 KWindowsDesktopInputDiagnostics::beginMouseMove(
	int nX, int nY, bool bDragging)
{
	if (!m_bEnabled)
		return 0;
	ensureObserversStarted();
	if (!bDragging)
		return 0;
	m_nLastDragX = nX;
	m_nLastDragY = nY;
	m_bHasPendingDrag = true;
	if (m_bFirstDragMoveLogged)
		return 0;
	m_bFirstDragMoveLogged = true;
	const quint64 nEventId = m_nNextEventId++;
	KDesktopInputTraceLogger::write(QStringLiteral("drag_first_before"),
		QStringLiteral("event=%1 interval=%2 %3")
			.arg(nEventId)
			.arg(m_nButtonStateInterval)
			.arg(PointSnapshot(nX, nY)));
	return nEventId;
}

void KWindowsDesktopInputDiagnostics::completeMouseMove(quint64 nEventId,
	unsigned int nInsertedInputs,
	unsigned long nWin32Error)
{
	if (!m_bEnabled || !m_bHasPendingDrag)
		return;
	m_nLastDragInsertedInputs = nInsertedInputs;
	m_nLastDragWin32Error = nWin32Error;
	if (nEventId == 0)
		return;
	KDesktopInputTraceLogger::write(QStringLiteral("drag_first_after"),
		QStringLiteral("event=%1 inserted=%2 error=%3 %4")
			.arg(nEventId)
			.arg(nInsertedInputs)
			.arg(nWin32Error)
			.arg(PointSnapshot(m_nLastDragX, m_nLastDragY)));
}

void KWindowsDesktopInputDiagnostics::reset()
{
	if (!m_bEnabled)
		return;
	writePendingDragLast();
	m_bFirstDragMoveLogged = false;
	m_nButtonStateInterval = 0;
	g_nLastButtonEventId.store(0);
	KDesktopInputTraceLogger::write(QStringLiteral("input_reset"));
}

KDesktopInputWindowKind KWindowsDesktopInputDiagnostics::classifyWindow(
	const QString &strClassName, const QString &strProcessFileName)
{
	if (strProcessFileName.contains(QStringLiteral("n0va"), Qt::CaseInsensitive))
		return N0vaDesktopInputWindowKind;
	if (strClassName.compare(QStringLiteral("SysListView32"),
		Qt::CaseInsensitive) == 0)
	{
		return DesktopListViewInputWindowKind;
	}
	if (strClassName.compare(QStringLiteral("SHELLDLL_DefView"),
		Qt::CaseInsensitive) == 0)
	{
		return ShellDefViewInputWindowKind;
	}
	if (strClassName.compare(QStringLiteral("WorkerW"), Qt::CaseInsensitive) == 0)
		return WorkerInputWindowKind;
	if (strClassName.compare(QStringLiteral("Progman"), Qt::CaseInsensitive) == 0)
		return ProgramManagerInputWindowKind;
	return OtherDesktopInputWindowKind;
}

bool KWindowsDesktopInputDiagnostics::isDesktopListViewHierarchy(
	const QString &strProcessFileName,
	const QStringList &ancestorClassNames)
{
	if (strProcessFileName.compare(
		QStringLiteral("explorer.exe"), Qt::CaseInsensitive) != 0)
	{
		return false;
	}
	bool bHasShellDefView = false;
	bool bHasDesktopRoot = false;
	for (const QString &strClassName : ancestorClassNames)
	{
		if (strClassName.compare(
			QStringLiteral("SHELLDLL_DefView"), Qt::CaseInsensitive) == 0)
			bHasShellDefView = true;
		if (strClassName.compare(QStringLiteral("WorkerW"), Qt::CaseInsensitive) == 0
			|| strClassName.compare(QStringLiteral("Progman"), Qt::CaseInsensitive) == 0)
			bHasDesktopRoot = true;
	}
	return bHasShellDefView && bHasDesktopRoot;
}

QString KWindowsDesktopInputDiagnostics::processFileNameToken(
	const QString &strProcessPath)
{
	QString strToken = QFileInfo(strProcessPath).fileName();
	if (strToken.isEmpty())
		return QStringLiteral("unknown");
	for (qsizetype i = 0; i < strToken.size(); ++i)
	{
		const QChar character = strToken.at(i);
		if (!character.isLetterOrNumber()
			&& character != QLatin1Char('.')
			&& character != QLatin1Char('_')
			&& character != QLatin1Char('-'))
		{
			strToken[i] = QLatin1Char('_');
		}
	}
	return strToken;
}

QString KWindowsDesktopInputDiagnostics::integrityLevelName(
	unsigned long nIntegrityRid)
{
	if (nIntegrityRid < SECURITY_MANDATORY_LOW_RID)
		return QStringLiteral("untrusted");
	if (nIntegrityRid < SECURITY_MANDATORY_MEDIUM_RID)
		return QStringLiteral("low");
	if (nIntegrityRid < SECURITY_MANDATORY_HIGH_RID)
		return QStringLiteral("medium");
	if (nIntegrityRid < SECURITY_MANDATORY_SYSTEM_RID)
		return QStringLiteral("high");
	if (nIntegrityRid < SECURITY_MANDATORY_PROTECTED_PROCESS_RID)
		return QStringLiteral("system");
	return QStringLiteral("protected");
}

QString KWindowsDesktopInputDiagnostics::mouseHookFlagsToken(
	unsigned long nFlags)
{
	return QStringLiteral("raw=0x%1 injected=%2 lowerIlInjected=%3")
		.arg(QString::number(nFlags, 16))
		.arg((nFlags & LLMHF_INJECTED) != 0 ? 1 : 0)
		.arg((nFlags & LLMHF_LOWER_IL_INJECTED) != 0 ? 1 : 0);
}

void KWindowsDesktopInputDiagnostics::ensureObserversStarted()
{
	if (!m_bEnabled || m_bObserversStarted)
		return;
	m_bObserversStarted = true;

	m_pWinEventHook = SetWinEventHook(EVENT_OBJECT_FOCUS,
		EVENT_OBJECT_SELECTIONWITHIN,
		nullptr,
		HandleDesktopWinEvent,
		0,
		0,
		WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

	bool bExpectedInactive = false;
	DWORD nMouseHookError = ERROR_SUCCESS;
	if (g_bMouseObserverActive.compare_exchange_strong(bExpectedInactive, true))
	{
		SetLastError(ERROR_SUCCESS);
		m_pMouseHook = SetWindowsHookExW(WH_MOUSE_LL,
			HandleLowLevelMouse,
			GetModuleHandleW(nullptr),
			0);
		if (m_pMouseHook == nullptr)
		{
			nMouseHookError = GetLastError();
			g_bMouseObserverActive.store(false);
		}
	}
	else
	{
		nMouseHookError = ERROR_BUSY;
	}

	const HWND hDesktopListView = DesktopListView();
	DWORD nExplorerProcessId = 0;
	GetWindowThreadProcessId(hDesktopListView, &nExplorerProcessId);
	KDesktopInputTraceLogger::write(QStringLiteral("observer_start"),
		QStringLiteral(
			"mouseHook=%1 mouseHookError=%2 winEventHook=%3 current={%4} explorer={%5} "
			"n0va={%6} desktop=%7")
			.arg(m_pMouseHook != nullptr ? 1 : 0)
			.arg(nMouseHookError)
			.arg(m_pWinEventHook != nullptr ? 1 : 0)
			.arg(ProcessSecurityToken(GetCurrentProcessId()))
			.arg(ProcessSecurityToken(nExplorerProcessId))
			.arg(RunningN0vaSecurityToken())
			.arg(WindowToken(hDesktopListView)));
}

void KWindowsDesktopInputDiagnostics::writePendingDragLast()
{
	if (!m_bEnabled || !m_bHasPendingDrag)
		return;
	KDesktopInputTraceLogger::write(QStringLiteral("drag_last"),
		QStringLiteral("interval=%1 inserted=%2 error=%3 %4")
			.arg(m_nButtonStateInterval)
			.arg(m_nLastDragInsertedInputs)
			.arg(m_nLastDragWin32Error)
			.arg(PointSnapshot(m_nLastDragX, m_nLastDragY)));
	m_bHasPendingDrag = false;
}
