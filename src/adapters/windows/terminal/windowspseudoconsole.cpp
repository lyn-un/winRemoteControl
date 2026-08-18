#include "adapters/windows/terminal/windowspseudoconsole.h"
#include "adapters/windows/terminal/powershelllaunchpolicy.h"

#include "common/sessiontracelogger.h"
#include "core/terminal/terminalwriteall.h"

#include <QtCore/QDir>
#include <QtCore/QMetaObject>
#include <QtCore/QStandardPaths>

#include <array>
#include <utility>

namespace
{
	constexpr qsizetype kMaximumInputQueueBytes = 256 * 1024;
	constexpr qsizetype kMaximumInputQueueMessages = 256;
	constexpr DWORD kProcessStopWaitMs = 2000;
	constexpr DWORD kWriteThreadStopWaitMs = 2000;
	constexpr DWORD kWriteThreadFallbackWaitMs = 2000;

	void CloseNativeHandle(HANDLE *pHandle)
	{
		if (pHandle != nullptr && *pHandle != nullptr && *pHandle != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(*pHandle);
			*pHandle = nullptr;
		}
	}
}

KWindowsPseudoConsole::KWindowsPseudoConsole(QObject *pParent)
	: KWindowsPseudoConsole(KNativeWriteFunction(), pParent)
{
}

KWindowsPseudoConsole::KWindowsPseudoConsole(
	KNativeWriteFunction writeFunction,
	QObject *pParent)
	: KTerminalHost(pParent)
	, m_spWriteCallbackGate(std::make_shared<KWriteCallbackGate>())
	, m_writeFunction(std::move(writeFunction))
{
	m_spWriteCallbackGate->pTarget = this;
}

KWindowsPseudoConsole::~KWindowsPseudoConsole()
{
	requestStop(m_nGeneration.load());
	if (m_teardownThread.joinable())
		m_teardownThread.join();
	std::lock_guard<std::mutex> gateGuard(m_spWriteCallbackGate->mutex);
	m_spWriteCallbackGate->pTarget = nullptr;
}

bool KWindowsPseudoConsole::isSupported(QString *pReason) const
{
	return loadFunctions(pReason);
}

bool KWindowsPseudoConsole::start(quint64 nGeneration,
	int nColumns,
	int nRows,
	QString *pErrorMessage)
{
	const std::shared_ptr<KWriteState> spPreviousWriteState =
		m_spWriteState.load();
	if (m_bRunning || m_bStopping
		|| (spPreviousWriteState != nullptr && !spPreviousWriteState->bFinished))
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = QStringLiteral(
				"Terminal is already active (running=%1 stopping=%2 writerFinished=%3)")
				.arg(m_bRunning.load() ? 1 : 0)
				.arg(m_bStopping.load() ? 1 : 0)
				.arg(spPreviousWriteState == nullptr
					|| spPreviousWriteState->bFinished ? 1 : 0);
		}
		return false;
	}
	if (!loadFunctions(pErrorMessage))
		return false;
	if (m_teardownThread.joinable())
		m_teardownThread.join();

	HANDLE hInputRead = nullptr;
	HANDLE hOutputWrite = nullptr;
	if (!::CreatePipe(&hInputRead, &m_hInputWrite, nullptr, 0)
		|| !::CreatePipe(&m_hOutputRead, &hOutputWrite, nullptr, 0))
	{
		CloseNativeHandle(&hInputRead);
		CloseNativeHandle(&hOutputWrite);
		resetHandles();
		if (pErrorMessage != nullptr)
			*pErrorMessage = windowsError(QStringLiteral("Create ConPTY pipes failed"), ::GetLastError());
		return false;
	}
	::SetHandleInformation(m_hInputWrite, HANDLE_FLAG_INHERIT, 0);
	::SetHandleInformation(m_hOutputRead, HANDLE_FLAG_INHERIT, 0);

	const COORD size = { static_cast<SHORT>(nColumns), static_cast<SHORT>(nRows) };
	const HRESULT hrConsole = ::CreatePseudoConsole(size,
		hInputRead, hOutputWrite, 0, &m_hPseudoConsole);
	if (FAILED(hrConsole))
	{
		CloseNativeHandle(&hInputRead);
		CloseNativeHandle(&hOutputWrite);
		resetHandles();
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("CreatePseudoConsole failed: 0x%1")
				.arg(static_cast<quint32>(hrConsole), 8, 16, QLatin1Char('0'));
		return false;
	}

	SIZE_T nAttributeBytes = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &nAttributeBytes);
	LPPROC_THREAD_ATTRIBUTE_LIST pAttributeList =
		reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
			::HeapAlloc(::GetProcessHeap(), 0, nAttributeBytes));
	if (pAttributeList == nullptr
		|| !::InitializeProcThreadAttributeList(pAttributeList, 1, 0, &nAttributeBytes))
	{
		if (pAttributeList != nullptr)
			::HeapFree(::GetProcessHeap(), 0, pAttributeList);
		m_pClosePseudoConsole(m_hPseudoConsole);
		m_hPseudoConsole = nullptr;
		CloseNativeHandle(&hInputRead);
		CloseNativeHandle(&hOutputWrite);
		resetHandles();
		if (pErrorMessage != nullptr)
			*pErrorMessage = windowsError(QStringLiteral("Initialize ConPTY attributes failed"), ::GetLastError());
		return false;
	}
	if (!::UpdateProcThreadAttribute(pAttributeList, 0,
			PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
			m_hPseudoConsole, sizeof(m_hPseudoConsole), nullptr, nullptr))
	{
		::DeleteProcThreadAttributeList(pAttributeList);
		::HeapFree(::GetProcessHeap(), 0, pAttributeList);
		m_pClosePseudoConsole(m_hPseudoConsole);
		m_hPseudoConsole = nullptr;
		CloseNativeHandle(&hInputRead);
		CloseNativeHandle(&hOutputWrite);
		resetHandles();
		if (pErrorMessage != nullptr)
			*pErrorMessage = windowsError(QStringLiteral("Initialize ConPTY attributes failed"), ::GetLastError());
		return false;
	}

	STARTUPINFOEXW startup = {};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullptr;
	startup.StartupInfo.hStdOutput = nullptr;
	startup.StartupInfo.hStdError = nullptr;
	startup.lpAttributeList = pAttributeList;
	PROCESS_INFORMATION process = {};
	QString strWindowsPowerShellPath;
	std::array<wchar_t, MAX_PATH> systemDirectory = {};
	const UINT nSystemDirectoryLength = ::GetSystemDirectoryW(
		systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
	if (nSystemDirectoryLength > 0 && nSystemDirectoryLength < systemDirectory.size())
	{
		strWindowsPowerShellPath = QDir(QString::fromWCharArray(systemDirectory.data()))
			.filePath(QStringLiteral("WindowsPowerShell/v1.0/powershell.exe"));
	}
	const QString strProgramFiles = qEnvironmentVariable("ProgramFiles");
	const QString strStandardPowerShell7Path = strProgramFiles.isEmpty()
		? QString()
		: QDir(strProgramFiles).filePath(QStringLiteral("PowerShell/7/pwsh.exe"));
	const QVector<KPowerShellCandidate> candidates = ResolvePowerShellCandidates(
		strStandardPowerShell7Path,
		QStandardPaths::findExecutable(QStringLiteral("pwsh.exe")),
		strWindowsPowerShellPath);
	const std::wstring strArguments =
		L"-NoLogo -NoExit -Command "
		L"\"[Console]::InputEncoding = [Text.UTF8Encoding]::new($false); "
		L"[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false); "
		L"$OutputEncoding = [Text.UTF8Encoding]::new($false)\"";
	const std::wstring strDirectory = QDir::toNativeSeparators(QDir::homePath()).toStdWString();
	const KPowerShellStartResult startResult = StartPowerShellCandidate(candidates,
		[&process, &startup, &strArguments, &strDirectory](
			const KPowerShellCandidate &candidate) -> quint32
		{
			const std::wstring strExecutable = QDir::toNativeSeparators(
				candidate.strExecutablePath).toStdWString();
			std::wstring strCommand = L"\"" + strExecutable + L"\" " + strArguments;
			PROCESS_INFORMATION attemptProcess = {};
			const BOOL bCreated = ::CreateProcessW(strExecutable.c_str(), strCommand.data(),
				nullptr, nullptr, FALSE,
				EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
				nullptr, strDirectory.c_str(), &startup.StartupInfo, &attemptProcess);
			if (bCreated)
			{
				process = attemptProcess;
				return ERROR_SUCCESS;
			}
			const DWORD nError = ::GetLastError();
			if (candidate.edition == PowerShell7Edition)
			{
				KSessionTraceLogger::write(QStringLiteral("controlled"),
					QStringLiteral("terminal_shell_start_failed"),
					QStringLiteral("state"), -1,
					QStringLiteral("edition=powershell7 error=%1").arg(nError));
			}
			return nError;
		});
	::DeleteProcThreadAttributeList(pAttributeList);
	::HeapFree(::GetProcessHeap(), 0, pAttributeList);
	CloseNativeHandle(&hInputRead);
	CloseNativeHandle(&hOutputWrite);
	if (!startResult.succeeded())
	{
		m_pClosePseudoConsole(m_hPseudoConsole);
		m_hPseudoConsole = nullptr;
		resetHandles();
		if (pErrorMessage != nullptr)
			*pErrorMessage = windowsError(QStringLiteral("Start PowerShell failed"),
				static_cast<DWORD>(startResult.nLastError));
		return false;
	}
	const KPowerShellCandidate &selectedCandidate = candidates.at(startResult.nSelectedIndex);
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("terminal_shell_selected"),
		QStringLiteral("state"), -1,
		QStringLiteral("edition=%1 fallback=%2")
			.arg(PowerShellEditionName(selectedCandidate.edition))
			.arg(selectedCandidate.bFallback ? 1 : 0));
	m_hProcess = process.hProcess;
	m_hJob = ::CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (m_hJob == nullptr
		|| !::SetInformationJobObject(m_hJob, JobObjectExtendedLimitInformation,
			&limits, sizeof(limits))
		|| !::AssignProcessToJobObject(m_hJob, m_hProcess)
		|| ::ResumeThread(process.hThread) == static_cast<DWORD>(-1))
	{
		const DWORD nError = ::GetLastError();
		::TerminateProcess(m_hProcess, 1);
		::WaitForSingleObject(m_hProcess, kProcessStopWaitMs);
		::CloseHandle(process.hThread);
		m_pClosePseudoConsole(m_hPseudoConsole);
		m_hPseudoConsole = nullptr;
		resetHandles();
		if (pErrorMessage != nullptr)
			*pErrorMessage = windowsError(QStringLiteral("Start PowerShell job failed"), nError);
		return false;
	}
	::CloseHandle(process.hThread);

	m_nGeneration = nGeneration;
	m_bStopping = false;
	m_bRunning = true;
	const std::shared_ptr<KWriteState> spWriteState =
		std::make_shared<KWriteState>();
	spWriteState->hInputWrite = m_hInputWrite;
	m_hInputWrite = nullptr;
	spWriteState->nGeneration = nGeneration;
	spWriteState->spCallbackGate = m_spWriteCallbackGate;
	spWriteState->writeFunction = m_writeFunction;
	if (!spWriteState->writeFunction)
	{
		spWriteState->writeFunction = [spWriteState](const char *pData,
			qsizetype nBytes, qsizetype *pBytesWritten, quint32 *pErrorCode)
		{
			DWORD nWritten = 0;
			const HANDLE hInputWrite = spWriteState->hInputWrite.load();
			const BOOL bWritten = hInputWrite != nullptr
				? ::WriteFile(hInputWrite, pData, static_cast<DWORD>(nBytes),
					&nWritten, nullptr)
				: FALSE;
			*pBytesWritten = static_cast<qsizetype>(nWritten);
			*pErrorCode = bWritten ? ERROR_SUCCESS
				: (hInputWrite != nullptr ? ::GetLastError() : ERROR_INVALID_HANDLE);
			return bWritten != FALSE;
		};
	}
	m_spWriteState.store(spWriteState);
	m_readThread = std::thread(&KWindowsPseudoConsole::readOutput, this, nGeneration);
	m_writeThread = std::thread(&KWindowsPseudoConsole::writeInputLoop,
		spWriteState);
	m_processThread = std::thread(&KWindowsPseudoConsole::waitForProcess, this, nGeneration);
	return true;
}

bool KWindowsPseudoConsole::writeInput(quint64 nGeneration, const QByteArray &data)
{
	if (!m_bRunning || m_bStopping || nGeneration != m_nGeneration.load()
		|| data.isEmpty() || data.size() > 64 * 1024)
	{
		return false;
	}
	const std::shared_ptr<KWriteState> spState =
		m_spWriteState.load();
	if (spState == nullptr || spState->bStopping || spState->bFinished)
		return false;
	std::lock_guard<std::mutex> guard(spState->mutex);
	if (static_cast<qsizetype>(spState->inputQueue.size())
			>= kMaximumInputQueueMessages
		|| spState->nQueuedInputBytes + data.size() > kMaximumInputQueueBytes)
		return false;
	spState->inputQueue.push_back(data);
	spState->nQueuedInputBytes += data.size();
	spState->inputCondition.notify_one();
	return true;
}

bool KWindowsPseudoConsole::resize(quint64 nGeneration, int nColumns, int nRows)
{
	if (!m_bRunning || m_bStopping || nGeneration != m_nGeneration.load()
		|| nColumns < 20 || nColumns > 400 || nRows < 5 || nRows > 200)
	{
		return false;
	}
	std::lock_guard<std::mutex> guard(m_consoleMutex);
	if (m_hPseudoConsole == nullptr || m_bStopping)
		return false;
	const COORD size = { static_cast<SHORT>(nColumns), static_cast<SHORT>(nRows) };
	return SUCCEEDED(m_pResizePseudoConsole(m_hPseudoConsole, size));
}

void KWindowsPseudoConsole::requestStop(quint64 nGeneration)
{
	if (!m_bRunning || nGeneration != m_nGeneration.load() || m_bStopping.exchange(true))
		return;
	const std::shared_ptr<KWriteState> spWriteState =
		m_spWriteState.load();
	if (spWriteState != nullptr)
	{
		spWriteState->bStopping = true;
		spWriteState->inputCondition.notify_all();
	}
	m_teardownThread = std::thread(&KWindowsPseudoConsole::teardown, this, nGeneration);
}

bool KWindowsPseudoConsole::loadFunctions(QString *pReason) const
{
	if (m_pCreatePseudoConsole != nullptr
		&& m_pResizePseudoConsole != nullptr
		&& m_pClosePseudoConsole != nullptr)
	{
		return true;
	}
	m_hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
	if (m_hKernel32 != nullptr)
	{
		m_pCreatePseudoConsole = reinterpret_cast<CreatePseudoConsoleFunction>(
			::GetProcAddress(m_hKernel32, "CreatePseudoConsole"));
		m_pResizePseudoConsole = reinterpret_cast<ResizePseudoConsoleFunction>(
			::GetProcAddress(m_hKernel32, "ResizePseudoConsole"));
		m_pClosePseudoConsole = reinterpret_cast<ClosePseudoConsoleFunction>(
			::GetProcAddress(m_hKernel32, "ClosePseudoConsole"));
	}
	const bool bSupported = m_pCreatePseudoConsole != nullptr
		&& m_pResizePseudoConsole != nullptr
		&& m_pClosePseudoConsole != nullptr;
	if (!bSupported && pReason != nullptr)
		*pReason = QStringLiteral("Windows ConPTY requires Windows 10 version 1809 or later");
	return bSupported;
}

void KWindowsPseudoConsole::readOutput(quint64 nGeneration)
{
	std::array<char, 16384> buffer = {};
	while (!m_bStopping && nGeneration == m_nGeneration.load())
	{
		DWORD nRead = 0;
		if (!::ReadFile(m_hOutputRead, buffer.data(), static_cast<DWORD>(buffer.size()),
			&nRead, nullptr) || nRead == 0)
		{
			break;
		}
		emit outputReady(nGeneration, QByteArray(buffer.data(), static_cast<qsizetype>(nRead)));
	}
	if (!m_bStopping && nGeneration == m_nGeneration.load())
	{
		QMetaObject::invokeMethod(this,
			[this, nGeneration]() { requestStop(nGeneration); },
			Qt::QueuedConnection);
	}
}

void KWindowsPseudoConsole::writeInputLoop(
	const std::shared_ptr<KWriteState> &spState)
{
	for (;;)
	{
		QByteArray data;
		{
			std::unique_lock<std::mutex> lock(spState->mutex);
			spState->inputCondition.wait(lock, [&spState]()
				{ return spState->bStopping || !spState->inputQueue.empty(); });
			if (spState->bStopping && spState->inputQueue.empty())
				break;
			data = std::move(spState->inputQueue.front());
			spState->inputQueue.pop_front();
			spState->nQueuedInputBytes -= data.size();
		}
		const KTerminalWriteResult result = WriteAllTerminalData(data,
			spState->writeFunction,
			[&spState]()
			{
				return !spState->bStopping;
			});
		if (!result.bSucceeded && !spState->bStopping)
		{
			postWriteFailure(spState, result.nErrorCode);
			break;
		}
	}
	spState->bFinished = true;
}

void KWindowsPseudoConsole::postWriteFailure(
	const std::shared_ptr<KWriteState> &spState,
	quint32 nErrorCode)
{
	const std::shared_ptr<KWriteCallbackGate> spGate = spState->spCallbackGate;
	if (spGate == nullptr)
		return;
	std::lock_guard<std::mutex> gateGuard(spGate->mutex);
	KWindowsPseudoConsole *pTarget = spGate->pTarget;
	if (pTarget == nullptr)
		return;
	QMetaObject::invokeMethod(pTarget,
		[spGate, nGeneration = spState->nGeneration, nErrorCode]()
		{
			KWindowsPseudoConsole *pLiveTarget = nullptr;
			{
				std::lock_guard<std::mutex> callbackGuard(spGate->mutex);
				pLiveTarget = spGate->pTarget;
			}
			if (pLiveTarget == nullptr)
				return;
			emit pLiveTarget->terminalError(nGeneration, QStringLiteral("write_failed"),
				windowsError(QStringLiteral("Write ConPTY input failed"),
					static_cast<DWORD>(nErrorCode)));
			pLiveTarget->requestStop(nGeneration);
		}, Qt::QueuedConnection);
}

void KWindowsPseudoConsole::waitForProcess(quint64 nGeneration)
{
	if (m_hProcess == nullptr)
		return;
	DWORD nWaitResult = WAIT_TIMEOUT;
	while (!m_bStopping && nGeneration == m_nGeneration.load()
		&& nWaitResult == WAIT_TIMEOUT)
	{
		nWaitResult = ::WaitForSingleObject(m_hProcess, 100);
	}
	if (!m_bStopping && nGeneration == m_nGeneration.load())
	{
		QMetaObject::invokeMethod(this,
			[this, nGeneration]() { requestStop(nGeneration); },
			Qt::QueuedConnection);
	}
}

void KWindowsPseudoConsole::teardown(quint64 nGeneration)
{
	DWORD nExitCode = STILL_ACTIVE;
	if (m_hProcess != nullptr)
		::GetExitCodeProcess(m_hProcess, &nExitCode);
	if (nExitCode == STILL_ACTIVE && m_hJob != nullptr)
		::TerminateJobObject(m_hJob, 1);
	if (nExitCode == STILL_ACTIVE && m_hProcess != nullptr)
		::TerminateProcess(m_hProcess, 1);
	const std::shared_ptr<KWriteState> spWriteState =
		m_spWriteState.load();
	if (spWriteState != nullptr)
	{
		spWriteState->bStopping = true;
		spWriteState->inputCondition.notify_all();
	}
	if (m_writeThread.joinable())
	{
		const HANDLE hWriteThread = m_writeThread.native_handle();
		if (!::CancelSynchronousIo(hWriteThread))
		{
			const DWORD nCancelError = ::GetLastError();
			if (nCancelError != ERROR_NOT_FOUND)
			{
				KSessionTraceLogger::write(QStringLiteral("controlled"),
					QStringLiteral("terminal_write_cancel_failed"),
					QStringLiteral("lifecycle"), -1,
					QStringLiteral("generation=%1 win32=%2")
						.arg(nGeneration).arg(nCancelError));
			}
		}
		if (::WaitForSingleObject(hWriteThread, kWriteThreadStopWaitMs) == WAIT_TIMEOUT)
		{
			KSessionTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("terminal_write_stop_timeout"),
				QStringLiteral("lifecycle"), -1,
				QStringLiteral("generation=%1 fallback=close_input")
					.arg(nGeneration));
			// Cancellation should normally release WriteFile. Closing the pipe is a
			// bounded last resort, used only after the writer ignored cancellation.
			if (spWriteState != nullptr)
			{
				HANDLE hInputWrite = spWriteState->hInputWrite.exchange(nullptr);
				CloseNativeHandle(&hInputWrite);
			}
		}
		const DWORD nFinalWait = ::WaitForSingleObject(hWriteThread,
			kWriteThreadFallbackWaitMs);
		if (nFinalWait == WAIT_OBJECT_0)
			m_writeThread.join();
		else
		{
			KSessionTraceLogger::write(QStringLiteral("controlled"),
				QStringLiteral("terminal_write_thread_stuck"),
				QStringLiteral("lifecycle"), -1,
				QStringLiteral("generation=%1 waitResult=%2")
					.arg(nGeneration).arg(nFinalWait));
			m_writeThread.detach();
		}
	}
	if (spWriteState != nullptr)
	{
		HANDLE hInputWrite = spWriteState->hInputWrite.exchange(nullptr);
		CloseNativeHandle(&hInputWrite);
	}
	if (m_hProcess != nullptr)
		::WaitForSingleObject(m_hProcess, kProcessStopWaitMs);
	if (m_processThread.joinable())
		m_processThread.join();
	closePseudoConsole();
	if (m_readThread.joinable())
		m_readThread.join();
	CloseNativeHandle(&m_hOutputRead);
	nExitCode = 0;
	if (m_hProcess != nullptr)
		::GetExitCodeProcess(m_hProcess, &nExitCode);
	resetHandles();
	if (spWriteState != nullptr)
	{
		std::lock_guard<std::mutex> guard(spWriteState->mutex);
		spWriteState->inputQueue.clear();
		spWriteState->nQueuedInputBytes = 0;
	}
	m_bRunning = false;
	m_bStopping = false;
	emit processExited(nGeneration, static_cast<int>(nExitCode));
	emit stopped(nGeneration);
}

void KWindowsPseudoConsole::closePseudoConsole()
{
	std::lock_guard<std::mutex> guard(m_consoleMutex);
	if (m_hPseudoConsole == nullptr)
		return;
	m_pClosePseudoConsole(m_hPseudoConsole);
	m_hPseudoConsole = nullptr;
}

void KWindowsPseudoConsole::resetHandles()
{
	CloseNativeHandle(&m_hInputWrite);
	CloseNativeHandle(&m_hOutputRead);
	CloseNativeHandle(&m_hProcess);
	CloseNativeHandle(&m_hJob);
}

QString KWindowsPseudoConsole::windowsError(const QString &strPrefix, DWORD nError)
{
	return QStringLiteral("%1 (win32=%2)").arg(strPrefix).arg(nError);
}
