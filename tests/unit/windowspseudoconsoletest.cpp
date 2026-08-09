#include "adapters/windows/terminal/windowspseudoconsole.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

#include <iostream>

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
	QObject::connect(&terminal, &KTerminalHost::outputReady,
		[&output](quint64, const QByteArray &data) { output.append(data); });
	QObject::connect(&terminal, &KTerminalHost::stopped,
		[&bStopped](quint64) { bStopped = true; });

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
		QByteArray("Write-Output WRC_CONPTY_OK; exit\r")))
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
	return 0;
}
