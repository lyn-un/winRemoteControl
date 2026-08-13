#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_WINDOWSPSEUDOCONSOLE_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_WINDOWSPSEUDOCONSOLE_H_

#include "core/terminal/terminalhost.h"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class KWindowsPseudoConsole final : public KTerminalHost
{
	Q_OBJECT

public:
	explicit KWindowsPseudoConsole(QObject *pParent = nullptr);
	~KWindowsPseudoConsole() override;

	KWindowsPseudoConsole(const KWindowsPseudoConsole &) = delete;
	KWindowsPseudoConsole &operator=(const KWindowsPseudoConsole &) = delete;

	bool isSupported(QString *pReason) const override;
	bool start(quint64 nGeneration, int nColumns, int nRows,
		QString *pErrorMessage) override;
	bool writeInput(quint64 nGeneration, const QByteArray &data) override;
	bool resize(quint64 nGeneration, int nColumns, int nRows) override;
	void requestStop(quint64 nGeneration) override;

private:
	using CreatePseudoConsoleFunction = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
	using ResizePseudoConsoleFunction = HRESULT(WINAPI *)(HPCON, COORD);
	using ClosePseudoConsoleFunction = void(WINAPI *)(HPCON);

	bool loadFunctions(QString *pReason) const;
	void readOutput(quint64 nGeneration);
	void writeInputLoop(quint64 nGeneration);
	void waitForProcess(quint64 nGeneration);
	void teardown(quint64 nGeneration);
	void closePseudoConsole();
	void resetHandles();
	static QString windowsError(const QString &strPrefix, DWORD nError);

	mutable HMODULE m_hKernel32 = nullptr;
	mutable CreatePseudoConsoleFunction m_pCreatePseudoConsole = nullptr;
	mutable ResizePseudoConsoleFunction m_pResizePseudoConsole = nullptr;
	mutable ClosePseudoConsoleFunction m_pClosePseudoConsole = nullptr;
	HPCON m_hPseudoConsole = nullptr;
	HANDLE m_hInputWrite = nullptr;
	HANDLE m_hOutputRead = nullptr;
	HANDLE m_hProcess = nullptr;
	HANDLE m_hJob = nullptr;
	std::thread m_readThread;
	std::thread m_writeThread;
	std::thread m_processThread;
	std::thread m_teardownThread;
	std::mutex m_mutex;
	std::mutex m_consoleMutex;
	std::condition_variable m_inputCondition;
	std::deque<QByteArray> m_inputQueue;
	qsizetype m_nQueuedInputBytes = 0;
	std::atomic<quint64> m_nGeneration = 0;
	std::atomic_bool m_bRunning = false;
	std::atomic_bool m_bStopping = false;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_WINDOWSPSEUDOCONSOLE_H_
