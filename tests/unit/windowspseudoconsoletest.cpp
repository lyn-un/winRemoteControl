#include "adapters/windows/terminal/windowspseudoconsole.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <iostream>
#include <thread>

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	KWindowsPseudoConsole terminal;
	QString strReason;
	if (!terminal.isSupported(&strReason))
	{
		std::cerr << strReason.toStdString() << '\n';
		return 0;
	}

	QByteArray output;
	bool bStopped = false;
	int nStoppedCount = 0;
	int nExitedCount = 0;
	QObject::connect(&terminal, &KTerminalHost::outputReady,
		[&output](quint64, const QByteArray &data) { output.append(data); });
	QObject::connect(&terminal, &KTerminalHost::stopped,
		[&bStopped, &nStoppedCount](quint64)
		{
			bStopped = true;
			++nStoppedCount;
		});
	QObject::connect(&terminal, &KTerminalHost::processExited,
		[&nExitedCount](quint64, int) { ++nExitedCount; });

	QString strError;
	if (!terminal.start(42, 100, 30, &strError))
	{
		std::cerr << strError.toStdString() << '\n';
		return 1;
	}
	QElapsedTimer timer;
	timer.start();
	while (!output.contains("PS ") && timer.elapsed() < 3000)
	{
		QCoreApplication::processEvents();
		QThread::msleep(10);
	}
	if (!terminal.writeInput(42,
		QByteArray("Write-Output WRC_CONPTY_OK; "
			"Write-Output ('WRC_PS_EDITION=' + $PSVersionTable.PSEdition); "
			"Write-Output ('WRC_INPUT_CODEPAGE=' + [Console]::InputEncoding.CodePage); "
			"Write-Output ('WRC_OUTPUT_CODEPAGE=' + [Console]::OutputEncoding.CodePage); "
			"Write-Output (([string][char]0x4E2D) + [char]0x6587); exit\r")))
	{
		return 1;
	}
	timer.restart();
	while (!bStopped && timer.elapsed() < 8000)
	{
		QCoreApplication::processEvents();
		QThread::msleep(10);
	}
	if (!bStopped)
	{
		std::cerr << "ConPTY did not stop, output bytes=" << output.size() << '\n';
		std::cerr.write(output.constData(), output.size());
		std::cerr << '\n';
		terminal.requestStop(42);
		return 1;
	}
	if (!output.contains("WRC_CONPTY_OK"))
	{
		std::cerr << "Missing marker, output bytes=" << output.size() << '\n';
		std::cerr.write(output.constData(), output.size());
		std::cerr << '\n';
		return 1;
	}
	if (!output.contains(QByteArray::fromHex("e4b8ade69687")))
	{
		std::cerr << "Missing UTF-8 marker, output bytes=" << output.size() << '\n';
		std::cerr.write(output.constData(), output.size());
		std::cerr << '\n';
		return 1;
	}
	if (!output.contains("WRC_INPUT_CODEPAGE=65001")
		|| !output.contains("WRC_OUTPUT_CODEPAGE=65001"))
	{
		std::cerr << "ConPTY is not configured for UTF-8, output bytes=" << output.size() << '\n';
		std::cerr.write(output.constData(), output.size());
		std::cerr << '\n';
		return 1;
	}
	if (!output.contains("WRC_PS_EDITION=Core")
		&& !output.contains("WRC_PS_EDITION=Desktop"))
	{
		std::cerr << "PowerShell edition marker is missing, output bytes=" << output.size() << '\n';
		std::cerr.write(output.constData(), output.size());
		std::cerr << '\n';
		return 1;
	}

	bStopped = false;
	output.clear();
	if (!terminal.start(43, 100, 30, &strError))
	{
		std::cerr << "ConPTY restart failed: " << strError.toStdString() << '\n';
		return 1;
	}
	std::thread resizeThread([&terminal]()
		{
			for (int nIndex = 0; nIndex < 200; ++nIndex)
			{
				terminal.resize(43, 80 + (nIndex % 40), 25 + (nIndex % 10));
				QThread::msleep(1);
			}
		});
	if (!terminal.writeInput(43,
		QByteArray("Write-Output WRC_RESTART_OK; Start-Sleep -Milliseconds 100; exit\r")))
	{
		resizeThread.join();
		return 1;
	}
	QTimer::singleShot(100, &terminal, [&terminal]() { terminal.requestStop(43); });
	timer.restart();
	while (!bStopped && timer.elapsed() < 5000)
	{
		QCoreApplication::processEvents();
		QThread::msleep(10);
	}
	resizeThread.join();
	if (!bStopped || nStoppedCount != 2 || nExitedCount != 2)
	{
		std::cerr << "ConPTY concurrent stop did not converge once per generation\n";
		return 1;
	}
	return 0;
}
