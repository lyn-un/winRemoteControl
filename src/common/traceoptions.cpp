#include "common/traceoptions.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>

namespace
{
	constexpr char kTraceOptionName[] = "trace";
	constexpr char kSessionTraceOptionName[] = "session-trace";
	constexpr char kLatencyTraceOptionName[] = "latency-trace";
	constexpr char kLogDirectoryOptionName[] = "log-dir";
	constexpr char kSessionTraceEnvironmentName[] = "WRC_SESSION_TRACE";
	constexpr char kLatencyTraceEnvironmentName[] = "WRC_LATENCY_TRACE";

	bool IsEnvironmentEnabled(const char *pName)
	{
		return qEnvironmentVariableIsSet(pName)
			&& qEnvironmentVariable(pName) != QStringLiteral("0");
	}
}

void KTraceOptionsParser::addOptions(QCommandLineParser *pParser)
{
	if (pParser == nullptr)
		return;

	pParser->addOption(QCommandLineOption(QString::fromLatin1(kTraceOptionName),
		QStringLiteral("Enable session and latency trace logs.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kSessionTraceOptionName),
		QStringLiteral("Enable session trace logs.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kLatencyTraceOptionName),
		QStringLiteral("Enable latency trace logs.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kLogDirectoryOptionName),
		QStringLiteral("Write trace logs to the specified directory."),
		QStringLiteral("directory")));
}

KTraceOptions KTraceOptionsParser::options(const QCommandLineParser &parser,
	const QString &strApplicationDirectory)
{
	const bool bEnableAll = parser.isSet(QString::fromLatin1(kTraceOptionName));
	KTraceOptions options;
	options.bSessionTraceEnabled = bEnableAll
		|| parser.isSet(QString::fromLatin1(kSessionTraceOptionName))
		|| IsEnvironmentEnabled(kSessionTraceEnvironmentName);
	options.bLatencyTraceEnabled = bEnableAll
		|| parser.isSet(QString::fromLatin1(kLatencyTraceOptionName))
		|| IsEnvironmentEnabled(kLatencyTraceEnvironmentName);

	const QString strRequestedDirectory = parser.value(
		QString::fromLatin1(kLogDirectoryOptionName)).trimmed();
	if (strRequestedDirectory.isEmpty())
	{
		options.strLogDirectory = QDir(strApplicationDirectory).absoluteFilePath(
			QStringLiteral("logs"));
	}
	else if (QDir::isAbsolutePath(strRequestedDirectory))
	{
		options.strLogDirectory = QDir::cleanPath(strRequestedDirectory);
	}
	else
	{
		options.strLogDirectory = QDir(strApplicationDirectory).absoluteFilePath(
			strRequestedDirectory);
	}
	return options;
}
