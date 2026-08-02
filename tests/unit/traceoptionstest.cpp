#include "common/traceoptions.h"
#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	KTraceOptions ParseOptions(const QStringList &arguments,
		const QString &strApplicationDirectory,
		bool *pParsed = nullptr)
	{
		QCommandLineParser parser;
		parser.addHelpOption();
		KTraceOptionsParser::addOptions(&parser);
		const bool bParsed = parser.parse(arguments);
		if (pParsed != nullptr)
			*pParsed = bParsed;
		return KTraceOptionsParser::options(parser, strApplicationDirectory);
	}

	void ClearTraceEnvironment()
	{
		qunsetenv("WRC_SESSION_TRACE");
		qunsetenv("WRC_LATENCY_TRACE");
	}

	void TestTraceOptions()
	{
		const QString strApplicationDirectory = QDir::cleanPath(
			QDir::temp().absoluteFilePath(QStringLiteral("wrc-app")));
		ClearTraceEnvironment();
		KTraceOptions options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe")}, strApplicationDirectory);
		Check(!options.bSessionTraceEnabled && !options.bLatencyTraceEnabled,
			QStringLiteral("trace logging is disabled by default"));
		Check(options.strLogDirectory == QDir(strApplicationDirectory).absoluteFilePath(
			QStringLiteral("logs")), QStringLiteral("default logs directory is beside executable"));

		options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe"), QStringLiteral("--trace")},
			strApplicationDirectory);
		Check(options.bSessionTraceEnabled && options.bLatencyTraceEnabled,
			QStringLiteral("--trace enables both loggers"));

		options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe"), QStringLiteral("--session-trace")},
			strApplicationDirectory);
		Check(options.bSessionTraceEnabled && !options.bLatencyTraceEnabled,
			QStringLiteral("--session-trace only enables session logging"));

		options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe"), QStringLiteral("--latency-trace")},
			strApplicationDirectory);
		Check(!options.bSessionTraceEnabled && options.bLatencyTraceEnabled,
			QStringLiteral("--latency-trace only enables latency logging"));

		qputenv("WRC_SESSION_TRACE", "1");
		qputenv("WRC_LATENCY_TRACE", "0");
		options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe")}, strApplicationDirectory);
		Check(options.bSessionTraceEnabled && !options.bLatencyTraceEnabled,
			QStringLiteral("existing environment variables remain supported"));
		ClearTraceEnvironment();

		const QString strAbsoluteDirectory = QDir::cleanPath(
			QDir::temp().absoluteFilePath(QStringLiteral("wrc-traces")));
		options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe"), QStringLiteral("--log-dir"),
				strAbsoluteDirectory}, strApplicationDirectory);
		Check(options.strLogDirectory == strAbsoluteDirectory,
			QStringLiteral("absolute log directory is preserved"));

		options = ParseOptions(
			{QStringLiteral("winRemoteControl.exe"), QStringLiteral("--log-dir"),
				QStringLiteral("diagnostics")}, strApplicationDirectory);
		Check(options.strLogDirectory == QDir(strApplicationDirectory).absoluteFilePath(
			QStringLiteral("diagnostics")),
			QStringLiteral("relative log directory is resolved beside executable"));

		bool bParsed = false;
		ParseOptions({QStringLiteral("winRemoteControl.exe"), QStringLiteral("--help")},
			strApplicationDirectory, &bParsed);
		Check(bParsed, QStringLiteral("--help is recognized"));
		ParseOptions({QStringLiteral("winRemoteControl.exe"), QStringLiteral("--unknown")},
			strApplicationDirectory, &bParsed);
		Check(!bParsed, QStringLiteral("unknown options are rejected"));
	}

	void TestConfiguredLogDirectory()
	{
		QTemporaryDir temporaryDirectory;
		Check(temporaryDirectory.isValid(), QStringLiteral("temporary log directory is available"));
		if (!temporaryDirectory.isValid())
			return;

		KSessionTraceLogger::configure(true, temporaryDirectory.path());
		KLatencyTraceLogger::configure(true, temporaryDirectory.path());
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("test"), QStringLiteral("configured_directory"));
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("configured_directory"));
		KLatencyTraceLogger::shutdown();

		Check(QFileInfo::exists(QDir(temporaryDirectory.path()).absoluteFilePath(
			QStringLiteral("session_trace_controlled.log"))),
			QStringLiteral("session trace keeps the controlled role in its file name"));
		Check(QFileInfo::exists(QDir(temporaryDirectory.path()).absoluteFilePath(
			QStringLiteral("latency_trace_controller.log"))),
			QStringLiteral("latency trace keeps the controller role in its file name"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestTraceOptions();
	TestConfiguredLogDirectory();
	ClearTraceEnvironment();
	if (g_nFailureCount == 0)
		qInfo() << "All trace options tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
