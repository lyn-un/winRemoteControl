#include <Windows.h>

#include "core/terminal/terminalrelayprotocol.h"

#include <atomic>
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

	void ConfigureInput(HANDLE hInput)
	{
		DWORD nMode = 0;
		if (!GetConsoleMode(hInput, &nMode))
			return;
		nMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
		nMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
		SetConsoleMode(hInput, nMode);
	}

	void InputLoop(HANDLE hPipe, HANDLE hInput)
	{
		std::vector<char> buffer(16 * 1024);
		while (g_bRunning.load())
		{
			DWORD nRead = 0;
			if (!ReadFile(hInput, buffer.data(), static_cast<DWORD>(buffer.size()),
				&nRead, nullptr) || nRead == 0)
			{
				break;
			}
			if (!SendFrame(hPipe, KTerminalRelayProtocol::InputFrameType,
				buffer.data(), nRead))
			{
				break;
			}
		}
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
	ConfigureInput(hInput);
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
