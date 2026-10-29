#include <Windows.h>
#include <windowsx.h>

#include <cstdlib>
#include <iostream>

namespace
{
constexpr wchar_t kWindowClass[] = L"WrcRemoteInputFixture";
unsigned int g_nClicks = 0;
unsigned int g_nWheelEvents = 0;
unsigned int g_nKeyDowns = 0;
unsigned int g_nCharacters = 0;
unsigned int g_nTicks = 0;

void Report()
{
	std::cout << "fixture_counts clicks=" << g_nClicks << " wheel=" << g_nWheelEvents
		<< " keyDowns=" << g_nKeyDowns << " characters=" << g_nCharacters
		<< " ticks=" << g_nTicks << std::endl;
}

LRESULT CALLBACK WindowProc(HWND hWindow, UINT nMessage, WPARAM wParam, LPARAM lParam)
{
	switch (nMessage)
	{
	case WM_LBUTTONUP:
		++g_nClicks;
		Report();
		return 0;
	case WM_MOUSEWHEEL:
		++g_nWheelEvents;
		Report();
		return 0;
	case WM_KEYDOWN:
		++g_nKeyDowns;
		Report();
		return 0;
	case WM_CHAR:
		++g_nCharacters;
		Report();
		return 0;
	case WM_SETCURSOR:
		if (LOWORD(lParam) == HTCLIENT)
		{
			POINT point = {};
			RECT client = {};
			GetCursorPos(&point);
			ScreenToClient(hWindow, &point);
			GetClientRect(hWindow, &client);
			const int nBand = point.y * 5 / (client.bottom > 0 ? client.bottom : 1);
			const LPCWSTR names[] = {IDC_ARROW, IDC_IBEAM, IDC_SIZEWE, IDC_WAIT};
			SetCursor(nBand >= 0 && nBand < 4 ? LoadCursorW(nullptr, names[nBand]) : nullptr);
			return TRUE;
		}
		break;
	case WM_TIMER:
		++g_nTicks;
		InvalidateRect(hWindow, nullptr, FALSE);
		if (g_nTicks >= 21000)
			DestroyWindow(hWindow);
		return 0;
	case WM_PAINT:
		{
			PAINTSTRUCT paint = {};
			HDC hDc = BeginPaint(hWindow, &paint);
			RECT client = {};
			GetClientRect(hWindow, &client);
			HBRUSH hBrush = CreateSolidBrush(RGB(235 + g_nTicks % 3 * 5, 242, 248));
			FillRect(hDc, &client, hBrush);
			DeleteObject(hBrush);
			const wchar_t *labels[] = {L"Blank input fixture - arrow", L"Text cursor",
				L"Resize cursor", L"Busy cursor", L"Hidden cursor"};
			SetBkMode(hDc, TRANSPARENT);
			for (int nBand = 0; nBand < 5; ++nBand)
			{
				RECT band = {20, client.bottom * nBand / 5, client.right - 20,
					client.bottom * (nBand + 1) / 5};
				DrawTextW(hDc, labels[nBand], -1, &band, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			}
			EndPaint(hWindow, &paint);
		}
		return 0;
	case WM_DESTROY:
		KillTimer(hWindow, 1);
		Report();
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hWindow, nMessage, wParam, lParam);
}
}

int main()
{
	char enabled[8] = {};
	size_t nLength = 0;
	if (getenv_s(&nLength, enabled, sizeof(enabled), "WRC_RUN_INPUT_E2E") != 0
		|| nLength != 2 || enabled[0] != '1')
	{
		return 77;
	}
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const HINSTANCE hInstance = GetModuleHandleW(nullptr);
	WNDCLASSW windowClass = {};
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = hInstance;
	windowClass.lpszClassName = kWindowClass;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	if (!RegisterClassW(&windowClass))
		return 1;
	const int nWidth = GetSystemMetrics(SM_CXSCREEN) * 3 / 4;
	const int nHeight = GetSystemMetrics(SM_CYSCREEN) * 3 / 4;
	HWND hWindow = CreateWindowExW(WS_EX_TOPMOST, kWindowClass,
		L"winRemoteControl blank remote input fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		(GetSystemMetrics(SM_CXSCREEN) - nWidth) / 2,
		(GetSystemMetrics(SM_CYSCREEN) - nHeight) / 2, nWidth, nHeight,
		nullptr, nullptr, hInstance, nullptr);
	if (!hWindow)
	{
		UnregisterClassW(kWindowClass, hInstance);
		return 1;
	}
	SetTimer(hWindow, 1, 100, nullptr);
	std::cout << "fixture_ready pid=" << GetCurrentProcessId()
		<< " dpi=" << GetDpiForWindow(hWindow) << " receiveOnly=1 autoCloseSeconds=2100" << std::endl;
	MSG message = {};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	UnregisterClassW(kWindowClass, hInstance);
	return 0;
}
