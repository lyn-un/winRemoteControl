#include "adapters/windows/input/windowsinputinjector.h"
#include "common/desktopinputtracelogger.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtGlobal>

#include <Windows.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace
{
	constexpr wchar_t kWindowClassName[] = L"WrcInputDoubleClickTestWindow";
	int g_nButtonDownCount = 0;
	int g_nButtonUpCount = 0;
	int g_nDoubleClickCount = 0;

	LRESULT CALLBACK TestWindowProc(HWND hWindow,
		UINT nMessage,
		WPARAM wParam,
		LPARAM lParam)
	{
		if (nMessage == WM_LBUTTONDOWN)
			++g_nButtonDownCount;
		else if (nMessage == WM_LBUTTONUP)
			++g_nButtonUpCount;
		else if (nMessage == WM_LBUTTONDBLCLK)
			++g_nDoubleClickCount;
		return DefWindowProcW(hWindow, nMessage, wParam, lParam);
	}

	void PumpMessages(int nDurationMs)
	{
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(nDurationMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			MSG message = {};
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	bool InjectMouseButton(KWindowsInputInjector *pInjector,
		HWND hTestWindow,
		int nX,
		int nY,
		bool bPressed)
	{
		DWORD dwProcessId = 0;
		GetWindowThreadProcessId(hTestWindow, &dwProcessId);
		const POINT target = {nX, nY};
		if (!IsWindow(hTestWindow) || dwProcessId != GetCurrentProcessId()
			|| GetForegroundWindow() != hTestWindow || WindowFromPoint(target) != hTestWindow)
		{
			std::cerr << "Input test window lost focus or target ownership; injection refused" << std::endl;
			return false;
		}
		KInputMessage message;
		message.type = MouseButtonInputMessageType;
		message.mouseButton = LeftRemoteMouseButton;
		message.nX = nX;
		message.nY = nY;
		message.bPressed = bPressed;
		QString strError;
		if (pInjector != nullptr && pInjector->inject(message, &strError))
			return true;
		std::cerr << strError.toStdString() << std::endl;
		return false;
	}

	QSet<QString> HookSequences(const QString &strTrace,
		const QString &strStage,
		bool bButtonEventsOnly)
	{
		QSet<QString> sequences;
		const QRegularExpression sequencePattern(
			QStringLiteral("stage=%1 hookSequence=(\\d+)")
				.arg(QRegularExpression::escape(strStage)));
		const QStringList lines = strTrace.split(QLatin1Char('\n'));
		for (const QString &strLine : lines)
		{
			if (bButtonEventsOnly
				&& (!strLine.contains(QStringLiteral("message=left_down"))
					&& !strLine.contains(QStringLiteral("message=left_up"))))
			{
				continue;
			}
			const QRegularExpressionMatch match = sequencePattern.match(strLine);
			if (match.hasMatch())
				sequences.insert(match.captured(1));
		}
		return sequences;
	}
}

int main(int nArgc, char *pArgv[])
{
	if (nArgc > 2 || (nArgc == 2 && std::strcmp(pArgv[1], "--wait-for-focus") != 0))
		return 2;
	const bool bWaitForFocus = nArgc == 2;
	if (qEnvironmentVariableIntValue("WRC_RUN_INPUT_E2E") != 1)
		return 77;
	QTemporaryDir traceDirectory;
	if (!traceDirectory.isValid())
	{
		std::cerr << "Unable to create the input trace directory" << std::endl;
		return 1;
	}
	KDesktopInputTraceLogger::configure(true, traceDirectory.path());

	POINT originalCursorPosition = {};
	GetCursorPos(&originalCursorPosition);
	HWND hOriginalForegroundWindow = GetForegroundWindow();
	HINSTANCE hInstance = GetModuleHandleW(nullptr);
	WNDCLASSW windowClass = {};
	windowClass.style = CS_DBLCLKS;
	windowClass.lpfnWndProc = TestWindowProc;
	windowClass.hInstance = hInstance;
	windowClass.lpszClassName = kWindowClassName;
	if (RegisterClassW(&windowClass) == 0)
	{
		std::cerr << "Unable to register the double-click test window" << std::endl;
		return 1;
	}

	const int nWindowWidth = 240;
	const int nWindowHeight = 160;
	const int nWindowX = qMax(0, (GetSystemMetrics(SM_CXSCREEN) - nWindowWidth) / 2);
	const int nWindowY = qMax(0, (GetSystemMetrics(SM_CYSCREEN) - nWindowHeight) / 2);
	HWND hWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		kWindowClassName,
		L"winRemoteControl input test",
		WS_POPUP | WS_VISIBLE,
		nWindowX,
		nWindowY,
		nWindowWidth,
		nWindowHeight,
		nullptr,
		nullptr,
		hInstance,
		nullptr);
	if (hWindow == nullptr)
	{
		UnregisterClassW(kWindowClassName, hInstance);
		std::cerr << "Unable to create the double-click test window" << std::endl;
		return 1;
	}

	ShowWindow(hWindow, SW_SHOW);
	UpdateWindow(hWindow);
	SetForegroundWindow(hWindow);
	PumpMessages(static_cast<int>(GetDoubleClickTime()) + 50);
	if (bWaitForFocus)
	{
		std::cout << "Waiting up to 30 seconds for normal activation of the scoped test window" << std::endl;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (GetForegroundWindow() != hWindow && std::chrono::steady_clock::now() < deadline)
			PumpMessages(25);
	}

	const int nTargetX = nWindowX + nWindowWidth / 2;
	const int nTargetY = nWindowY + nWindowHeight / 2;
	bool bSuccess = false;
	{
		KWindowsInputInjector injector;
		bSuccess = InjectMouseButton(&injector, hWindow, nTargetX, nTargetY, true)
			&& InjectMouseButton(&injector, hWindow, nTargetX, nTargetY, false);
		PumpMessages(50);
		bSuccess = bSuccess
			&& InjectMouseButton(&injector, hWindow, nTargetX, nTargetY, true)
			&& InjectMouseButton(&injector, hWindow, nTargetX, nTargetY, false);
		PumpMessages(200);
	}
	KDesktopInputTraceLogger::shutdown();
	QFile traceFile(QDir(traceDirectory.path()).absoluteFilePath(
		QStringLiteral("desktop_input_trace_controlled.log")));
	const bool bTraceReadable = traceFile.open(QIODevice::ReadOnly | QIODevice::Text);
	const QByteArray traceContents = bTraceReadable ? traceFile.readAll() : QByteArray();
	const QString strTrace = QString::fromUtf8(traceContents);
	const QSet<QString> buttonEnterSequences = HookSequences(
		strTrace, QStringLiteral("hook_enter"), true);
	const QSet<QString> exitSequences = HookSequences(
		strTrace, QStringLiteral("hook_exit"), false);
	QSet<QString> unmatchedSequences = buttonEnterSequences;
	unmatchedSequences.subtract(exitSequences);
	bSuccess = bSuccess
		&& buttonEnterSequences.size() == 4
		&& unmatchedSequences.isEmpty()
		&& traceContents.contains("injected=1")
		&& !traceContents.contains("stage=low_level_mouse");

	bSuccess = bSuccess
		&& g_nButtonDownCount == 1
		&& g_nButtonUpCount == 2
		&& g_nDoubleClickCount == 1;
	if (!bSuccess)
	{
		std::cerr << "Unexpected messages: down=" << g_nButtonDownCount
			<< " up=" << g_nButtonUpCount
			<< " doubleClick=" << g_nDoubleClickCount
			<< " traceReadable=" << bTraceReadable
			<< " buttonEnters=" << buttonEnterSequences.size()
			<< " unmatchedExits=" << unmatchedSequences.size()
			<< " injectedTrace=" << traceContents.contains("injected=1") << std::endl;
	}

	DestroyWindow(hWindow);
	UnregisterClassW(kWindowClassName, hInstance);
	SetCursorPos(originalCursorPosition.x, originalCursorPosition.y);
	if (hOriginalForegroundWindow != nullptr)
		SetForegroundWindow(hOriginalForegroundWindow);
	return bSuccess ? 0 : 1;
}
