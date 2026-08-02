#include "common/sessiontracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QTextStream>

namespace
{
	constexpr qint64 kSessionTraceMaxFileSize = 5 * 1024 * 1024;
	constexpr char kSessionTraceEnvName[] = "WRC_SESSION_TRACE";

	static QMutex g_sessionTraceMutex;
	static bool g_bSessionTraceInitialized = false;
	static bool g_bSessionTraceEnabled = false;
	static bool g_bSessionTraceConfigured = false;
	static bool g_bConfiguredSessionTraceEnabled = false;
	static QString g_strSessionTraceLogDirectory;

	static QString logDirectoryPath()
	{
		if (!g_strSessionTraceLogDirectory.isEmpty())
			return g_strSessionTraceLogDirectory;
		const QString strBasePath = QCoreApplication::applicationDirPath().isEmpty()
			? QDir::currentPath()
			: QCoreApplication::applicationDirPath();
		return QDir(strBasePath).absoluteFilePath(QStringLiteral("logs"));
	}

	static QString logFilePath(const QString &strRole)
	{
		return QDir(logDirectoryPath()).absoluteFilePath(
			QStringLiteral("session_trace_%1.log").arg(strRole));
	}

	static void initialize()
	{
		if (g_bSessionTraceInitialized)
			return;

		g_bSessionTraceInitialized = true;
		g_bSessionTraceEnabled = g_bSessionTraceConfigured
			? g_bConfiguredSessionTraceEnabled
			: qEnvironmentVariableIsSet(kSessionTraceEnvName)
				&& qEnvironmentVariable(kSessionTraceEnvName) != QStringLiteral("0");
		if (!g_bSessionTraceEnabled)
			return;

		const QString strLogDirectory = logDirectoryPath();
		if (!QDir().mkpath(strLogDirectory))
		{
			qWarning().noquote() << QStringLiteral("Unable to create trace log directory: %1")
				.arg(strLogDirectory);
			g_bSessionTraceEnabled = false;
		}
	}

	static void rotateIfNeeded(const QString &strFilePath)
	{
		QFile file(strFilePath);
		if (!file.exists() || file.size() < kSessionTraceMaxFileSize)
			return;

		const QString strOldFilePath = strFilePath + QStringLiteral(".old");
		QFile::remove(strOldFilePath);
		file.rename(strOldFilePath);
	}
}

void KSessionTraceLogger::configure(bool bEnabled, const QString &strLogDirectory)
{
	QMutexLocker locker(&g_sessionTraceMutex);
	if (g_bSessionTraceInitialized)
	{
		qWarning() << "Session trace logger was configured after initialization";
		return;
	}
	g_bSessionTraceConfigured = true;
	g_bConfiguredSessionTraceEnabled = bEnabled;
	g_strSessionTraceLogDirectory = QDir::cleanPath(strLogDirectory);
}

bool KSessionTraceLogger::isEnabled()
{
	QMutexLocker locker(&g_sessionTraceMutex);
	initialize();
	return g_bSessionTraceEnabled;
}

void KSessionTraceLogger::write(const QString &strRole,
	const QString &strStage,
	const QString &strType,
	int nSize,
	const QString &strExtra)
{
	QMutexLocker locker(&g_sessionTraceMutex);
	initialize();
	if (!g_bSessionTraceEnabled)
		return;

	const QString strFilePath = logFilePath(strRole);
	rotateIfNeeded(strFilePath);

	QFile file(strFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
	{
		qWarning().noquote() << QStringLiteral("Unable to open session trace log: %1")
			.arg(strFilePath);
		g_bSessionTraceEnabled = false;
		return;
	}

	QTextStream stream(&file);
	stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
		   << QStringLiteral(" [SESSION_TRACE]")
		   << QStringLiteral(" role=") << strRole
		   << QStringLiteral(" stage=") << strStage
		   << QStringLiteral(" type=") << strType;
	if (nSize >= 0)
		stream << QStringLiteral(" size=") << nSize;
	if (!strExtra.isEmpty())
		stream << QLatin1Char(' ') << strExtra;
	stream << Qt::endl;
}
