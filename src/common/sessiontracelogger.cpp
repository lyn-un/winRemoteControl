#include "common/sessiontracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
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

	static QString logDirectoryPath()
	{
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
		g_bSessionTraceEnabled = qEnvironmentVariableIsSet(kSessionTraceEnvName)
			&& qEnvironmentVariable(kSessionTraceEnvName) != QStringLiteral("0");
		if (!g_bSessionTraceEnabled)
			return;

		QDir().mkpath(logDirectoryPath());
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
		return;

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
