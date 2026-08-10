#include <Windows.h>

#include "core/terminal/terminalrelayprotocol.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::atomic_bool g_bRunning = true;
	std::mutex g_writeMutex;

	bool ReadExact(HANDLE hFile, void *pData, DWORD nBytes)
	{
		char *pCurrent = static_cast<char *>(pData);
		while (nBytes > 0 && g_bRunning.load())
		{
			DWORD nRead = 0;
			if (!ReadFile(hFile, pCurrent, nBytes, &nRead, nullptr) || nRead == 0)
				return false;
			pCurrent += nRead;
			nBytes -= nRead;
		}
		return nBytes == 0;
	}

	bool WriteExact(HANDLE hFile, const void *pData, DWORD nBytes)
	{
		const char *pCurrent = static_cast<const char *>(pData);
		while (nBytes > 0 && g_bRunning.load())
		{
			DWORD nWritten = 0;
			if (!WriteFile(hFile, pCurrent, nBytes, &nWritten, nullptr) || nWritten == 0)
				return false;
			pCurrent += nWritten;
			nBytes -= nWritten;
		}
		return nBytes == 0;
	}

	bool SendFrame(HANDLE hPipe, std::uint16_t nType,
		const void *pPayload = nullptr, std::uint32_t nPayloadBytes = 0)
	{
		if (nPayloadBytes > KTerminalRelayProtocol::kMaximumPayloadBytes)
			return false;
		KTerminalRelayProtocol::FrameHeader header;
		header.nType = nType;
		header.nPayloadBytes = nPayloadBytes;
		std::scoped_lock lock(g_writeMutex);
		return WriteExact(hPipe, &header, sizeof(header))
			&& (nPayloadBytes == 0 || WriteExact(hPipe, pPayload, nPayloadBytes));
	}

	BOOL WINAPI ConsoleControlHandler(DWORD nControlType)
	{
		if (nControlType == CTRL_C_EVENT || nControlType == CTRL_BREAK_EVENT)
			return TRUE;
		g_bRunning.store(false);
		return FALSE;
	}

	std::wstring ArgumentValue(int argc, wchar_t *argv[], const wchar_t *pName)
	{
		for (int nIndex = 1; nIndex + 1 < argc; ++nIndex)
		{
			if (std::wcscmp(argv[nIndex], pName) == 0)
				return argv[nIndex + 1];
		}
		return {};
	}

	std::string WideToUtf8(const std::wstring &value)
	{
		if (value.empty())
			return {};
		const int nBytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
			static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<std::size_t>(nBytes), '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
			result.data(), nBytes, nullptr, nullptr);
		return result;
	}

	void AppendBytes(std::vector<char> *pOutput, const char *pData,
		std::size_t nBytes, WORD nRepeatCount)
	{
		if (pOutput == nullptr || pData == nullptr || nBytes == 0)
			return;
		for (WORD nRepeat = 0; nRepeat < nRepeatCount; ++nRepeat)
			pOutput->insert(pOutput->end(), pData, pData + nBytes);
	}

	void AppendSequence(std::vector<char> *pOutput, const char *pSequence,
		WORD nRepeatCount)
	{
		AppendBytes(pOutput, pSequence, std::strlen(pSequence), nRepeatCount);
	}

	void AppendUnicodeCharacter(std::vector<char> *pOutput, wchar_t nCharacter,
		WORD nRepeatCount, wchar_t *pPendingHighSurrogate)
	{
		if (pOutput == nullptr || pPendingHighSurrogate == nullptr)
			return;
		if (nCharacter >= 0xD800 && nCharacter <= 0xDBFF)
		{
			*pPendingHighSurrogate = nCharacter;
			return;
		}

		std::array<wchar_t, 2> characters = {};
		int nCharacterCount = 1;
		if (nCharacter >= 0xDC00 && nCharacter <= 0xDFFF
			&& *pPendingHighSurrogate != 0)
		{
			characters[0] = *pPendingHighSurrogate;
			characters[1] = nCharacter;
			nCharacterCount = 2;
		}
		else
		{
			characters[0] = nCharacter;
		}
		*pPendingHighSurrogate = 0;

		const int nUtf8Bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			characters.data(), nCharacterCount, nullptr, 0, nullptr, nullptr);
		if (nUtf8Bytes <= 0)
			return;
		std::array<char, 8> utf8 = {};
		WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, characters.data(),
			nCharacterCount, utf8.data(), nUtf8Bytes, nullptr, nullptr);
		AppendBytes(pOutput, utf8.data(), static_cast<std::size_t>(nUtf8Bytes), nRepeatCount);
	}

	void EncodeKeyEvent(const KEY_EVENT_RECORD &event, std::vector<char> *pOutput,
		wchar_t *pPendingHighSurrogate)
	{
		if (!event.bKeyDown || pOutput == nullptr || pPendingHighSurrogate == nullptr)
			return;
		const WORD nRepeatCount = event.wRepeatCount == 0 ? 1 : event.wRepeatCount;
		const bool bShift = (event.dwControlKeyState & SHIFT_PRESSED) != 0;
		switch (event.wVirtualKeyCode)
		{
		case VK_BACK:
			AppendSequence(pOutput, "\x7f", nRepeatCount);
			return;
		case VK_RETURN:
			AppendSequence(pOutput, "\r", nRepeatCount);
			return;
		case VK_TAB:
			AppendSequence(pOutput, bShift ? "\x1b[Z" : "\t", nRepeatCount);
			return;
		case VK_ESCAPE:
			AppendSequence(pOutput, "\x1b", nRepeatCount);
			return;
		case VK_UP:
			AppendSequence(pOutput, "\x1b[A", nRepeatCount);
			return;
		case VK_DOWN:
			AppendSequence(pOutput, "\x1b[B", nRepeatCount);
			return;
		case VK_RIGHT:
			AppendSequence(pOutput, "\x1b[C", nRepeatCount);
			return;
		case VK_LEFT:
			AppendSequence(pOutput, "\x1b[D", nRepeatCount);
			return;
		case VK_HOME:
			AppendSequence(pOutput, "\x1b[H", nRepeatCount);
			return;
		case VK_END:
			AppendSequence(pOutput, "\x1b[F", nRepeatCount);
			return;
		case VK_INSERT:
			AppendSequence(pOutput, "\x1b[2~", nRepeatCount);
			return;
		case VK_DELETE:
			AppendSequence(pOutput, "\x1b[3~", nRepeatCount);
			return;
		case VK_PRIOR:
			AppendSequence(pOutput, "\x1b[5~", nRepeatCount);
			return;
		case VK_NEXT:
			AppendSequence(pOutput, "\x1b[6~", nRepeatCount);
			return;
		default:
			break;
		}
		if (event.uChar.UnicodeChar == 0)
			return;
		const bool bAlt = (event.dwControlKeyState
			& (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
		if (bAlt)
			AppendSequence(pOutput, "\x1b", 1);
		AppendUnicodeCharacter(pOutput, event.uChar.UnicodeChar,
			nRepeatCount, pPendingHighSurrogate);
	}

	bool ConfigureConsoleInput(HANDLE hInput)
	{
		DWORD nMode = 0;
		if (!GetConsoleMode(hInput, &nMode))
			return false;
		nMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT
			| ENABLE_ECHO_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
		return SetConsoleMode(hInput, nMode) != FALSE;
	}

	void ConsoleInputLoop(HANDLE hPipe, HANDLE hInput)
	{
		std::array<INPUT_RECORD, 128> records = {};
		wchar_t nPendingHighSurrogate = 0;
		while (g_bRunning.load())
		{
			DWORD nRead = 0;
			if (!ReadConsoleInputW(hInput, records.data(),
				static_cast<DWORD>(records.size()), &nRead) || nRead == 0)
			{
				break;
			}
			std::vector<char> output;
			for (DWORD nIndex = 0; nIndex < nRead; ++nIndex)
			{
				if (records[nIndex].EventType == KEY_EVENT)
					EncodeKeyEvent(records[nIndex].Event.KeyEvent,
						&output, &nPendingHighSurrogate);
			}
			if (!output.empty()
				&& !SendFrame(hPipe, KTerminalRelayProtocol::InputFrameType,
					output.data(), static_cast<std::uint32_t>(output.size())))
			{
				break;
			}
		}
	}

	void StreamInputLoop(HANDLE hPipe, HANDLE hInput)
	{
		std::vector<char> input(16 * 1024);
		while (g_bRunning.load())
		{
			DWORD nRead = 0;
			if (!ReadFile(hInput, input.data(), static_cast<DWORD>(input.size()),
				&nRead, nullptr) || nRead == 0)
			{
				break;
			}
			if (!SendFrame(hPipe, KTerminalRelayProtocol::InputFrameType,
				input.data(), nRead))
			{
				break;
			}
		}
	}

	void InputLoop(HANDLE hPipe, HANDLE hInput)
	{
		if (ConfigureConsoleInput(hInput))
			ConsoleInputLoop(hPipe, hInput);
		else
			StreamInputLoop(hPipe, hInput);
		g_bRunning.store(false);
	}

	void ResizeLoop(HANDLE hPipe, HANDLE hOutput)
	{
		SHORT nLastColumns = 0;
		SHORT nLastRows = 0;
		while (g_bRunning.load())
		{
			CONSOLE_SCREEN_BUFFER_INFO info = {};
			if (GetConsoleScreenBufferInfo(hOutput, &info))
			{
				const SHORT nColumns = info.srWindow.Right - info.srWindow.Left + 1;
				const SHORT nRows = info.srWindow.Bottom - info.srWindow.Top + 1;
				if ((nColumns != nLastColumns || nRows != nLastRows)
					&& nColumns >= 20 && nColumns <= 400 && nRows >= 5 && nRows <= 200)
				{
					KTerminalRelayProtocol::ResizePayload resize;
					resize.nColumns = static_cast<std::uint16_t>(nColumns);
					resize.nRows = static_cast<std::uint16_t>(nRows);
					if (!SendFrame(hPipe, KTerminalRelayProtocol::ResizeFrameType,
						&resize, sizeof(resize)))
					{
						break;
					}
					nLastColumns = nColumns;
					nLastRows = nRows;
				}
			}
			Sleep(100);
		}
	}

	void OutputLoop(HANDLE hPipe, HANDLE hOutput)
	{
		while (g_bRunning.load())
		{
			KTerminalRelayProtocol::FrameHeader header;
			if (!ReadExact(hPipe, &header, sizeof(header)))
				break;
			if (header.nMagic != KTerminalRelayProtocol::kMagic
				|| header.nVersion != KTerminalRelayProtocol::kVersion
				|| header.nPayloadBytes > KTerminalRelayProtocol::kMaximumPayloadBytes)
			{
				break;
			}
			std::vector<char> payload(header.nPayloadBytes);
			if (header.nPayloadBytes > 0
				&& !ReadExact(hPipe, payload.data(), header.nPayloadBytes))
			{
				break;
			}
			if (header.nType == KTerminalRelayProtocol::OutputFrameType)
			{
				if (!WriteExact(hOutput, payload.data(), header.nPayloadBytes))
					break;
			}
			else if (header.nType == KTerminalRelayProtocol::CloseFrameType)
			{
				break;
			}
			else
			{
				break;
			}
		}
		g_bRunning.store(false);
	}
}

int wmain(int argc, wchar_t *argv[])
{
	const std::wstring strPipeName = ArgumentValue(argc, argv, L"--pipe");
	const std::wstring strToken = ArgumentValue(argc, argv, L"--token");
	if (strPipeName.empty() || strToken.empty())
		return 2;

	SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
	const std::wstring strPipePath = L"\\\\.\\pipe\\" + strPipeName;
	if (!WaitNamedPipeW(strPipePath.c_str(), 10000))
		return 3;
	HANDLE hPipe = CreateFileW(strPipePath.c_str(), GENERIC_READ | GENERIC_WRITE,
		0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (hPipe == INVALID_HANDLE_VALUE)
		return 4;

	const std::string strTokenUtf8 = WideToUtf8(strToken);
	if (!SendFrame(hPipe, KTerminalRelayProtocol::HelloFrameType,
		strTokenUtf8.data(), static_cast<std::uint32_t>(strTokenUtf8.size())))
	{
		CloseHandle(hPipe);
		return 5;
	}

	HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	std::thread inputThread(InputLoop, hPipe, hInput);
	std::thread resizeThread(ResizeLoop, hPipe, hOutput);
	OutputLoop(hPipe, hOutput);
	g_bRunning.store(false);
	CancelSynchronousIo(inputThread.native_handle());
	CancelIoEx(hPipe, nullptr);
	if (inputThread.joinable())
		inputThread.join();
	if (resizeThread.joinable())
		resizeThread.join();
	SendFrame(hPipe, KTerminalRelayProtocol::CloseFrameType);
	CloseHandle(hPipe);
	return 0;
}
