#include "common/traceoptions.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>

namespace
{
	constexpr char kTraceOptionName[] = "trace";
	constexpr char kSessionTraceOptionName[] = "session-trace";
	constexpr char kLatencyTraceOptionName[] = "latency-trace";
	constexpr char kResourceTraceOptionName[] = "resource-trace";
	constexpr char kDesktopInputTraceOptionName[] = "desktop-input-trace";
	constexpr char kLatencyScenarioOptionName[] = "latency-scenario";
	constexpr char kLogDirectoryOptionName[] = "log-dir";
	constexpr char kSessionTraceEnvironmentName[] = "WRC_SESSION_TRACE";
	constexpr char kLatencyTraceEnvironmentName[] = "WRC_LATENCY_TRACE";
	constexpr char kResourceTraceEnvironmentName[] = "WRC_RESOURCE_TRACE";

	bool IsEnvironmentEnabled(const char *pName)
	{
		return qEnvironmentVariableIsSet(pName)
			&& qEnvironmentVariable(pName) != QStringLiteral("0");
	}

	bool IsSupportedLatencyScenario(const QString &strScenario)
	{
		return strScenario == QStringLiteral("static")
			|| strScenario == QStringLiteral("mouse")
			|| strScenario == QStringLiteral("window");
	}
}

void KTraceOptionsParser::addOptions(QCommandLineParser *pParser)
{
	if (pParser == nullptr)
		return;
	pParser->addOption(QCommandLineOption(QStringLiteral("video-renderer"),
		QStringLiteral("Receiver renderer: auto or bgra."), QStringLiteral("mode"), QStringLiteral("auto")));

	pParser->addOption(QCommandLineOption(QString::fromLatin1(kTraceOptionName),
		QStringLiteral("Enable all developer trace logs.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kSessionTraceOptionName),
		QStringLiteral("Enable session trace logs.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kLatencyTraceOptionName),
		QStringLiteral("Enable latency trace logs.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kResourceTraceOptionName),
		QStringLiteral("Enable process resource trace logs.")));
	pParser->addOption(QCommandLineOption(
		QString::fromLatin1(kDesktopInputTraceOptionName),
		QStringLiteral("Enable Windows desktop input diagnostics.")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kLatencyScenarioOptionName),
		QStringLiteral("Label a latency run as static, mouse, or window."),
		QStringLiteral("scenario")));
	pParser->addOption(QCommandLineOption(QString::fromLatin1(kLogDirectoryOptionName),
		QStringLiteral("Write trace logs to the specified directory."),
		QStringLiteral("directory")));
}

KTraceOptions KTraceOptionsParser::options(const QCommandLineParser &parser,
	const QString &strApplicationDirectory)
{
	const bool bEnableAll = parser.isSet(QString::fromLatin1(kTraceOptionName));
	KTraceOptions options;
	const QString strRenderer = parser.value(QStringLiteral("video-renderer"));
	options.bForceBgraVideoRenderer = strRenderer == QStringLiteral("bgra");
	if (strRenderer != QStringLiteral("auto") && strRenderer != QStringLiteral("bgra"))
		options.strValidationError = QStringLiteral("Invalid video renderer. Expected auto or bgra.");
	options.strLatencyScenario = parser.value(
		QString::fromLatin1(kLatencyScenarioOptionName)).trimmed().toLower();
	if (!options.strLatencyScenario.isEmpty()
		&& !IsSupportedLatencyScenario(options.strLatencyScenario))
	{
		options.strValidationError = QStringLiteral(
			"Invalid latency scenario '%1'. Expected static, mouse, or window.")
			.arg(options.strLatencyScenario);
	}
	options.bSessionTraceEnabled = bEnableAll
		|| parser.isSet(QString::fromLatin1(kSessionTraceOptionName))
		|| IsEnvironmentEnabled(kSessionTraceEnvironmentName);
	options.bLatencyTraceEnabled = bEnableAll
		|| parser.isSet(QString::fromLatin1(kLatencyTraceOptionName))
		|| !options.strLatencyScenario.isEmpty()
		|| IsEnvironmentEnabled(kLatencyTraceEnvironmentName);
	options.bResourceTraceEnabled = bEnableAll
		|| parser.isSet(QString::fromLatin1(kResourceTraceOptionName))
		|| IsEnvironmentEnabled(kResourceTraceEnvironmentName);
	options.bDesktopInputTraceEnabled = bEnableAll
		|| parser.isSet(QString::fromLatin1(kDesktopInputTraceOptionName));

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
