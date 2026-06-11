#include "common/latencytracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QTextStream>

namespace
{
	constexpr qint64 kLatencyTraceMaxFileSize = 5 * 1024 * 1024;
	constexpr char kLatencyTraceEnvName[] = "WRC_LATENCY_TRACE";

	static QMutex g_latencyTraceMutex;
	static bool g_bLatencyTraceInitialized = false;
	static bool g_bLatencyTraceEnabled = false;

	static QString logFilePath()
	{
		const QString strBasePath = QCoreApplication::applicationDirPath().isEmpty()
			? QDir::currentPath()
			: QCoreApplication::applicationDirPath();
		return QDir(strBasePath).absoluteFilePath(QStringLiteral("logs/latency_trace.log"));
	}

	static void initialize()
	{
		if (g_bLatencyTraceInitialized)
			return;

		g_bLatencyTraceInitialized = true;
		g_bLatencyTraceEnabled = qEnvironmentVariableIsSet(kLatencyTraceEnvName)
			&& qEnvironmentVariable(kLatencyTraceEnvName) != QStringLiteral("0");
		if (!g_bLatencyTraceEnabled)
			return;

		const QFileInfo fileInfo(logFilePath());
		QDir().mkpath(fileInfo.absolutePath());
	}

	static void rotateIfNeeded(const QString &strFilePath)
	{
		QFile file(strFilePath);
		if (!file.exists() || file.size() < kLatencyTraceMaxFileSize)
			return;

		const QString strOldFilePath = strFilePath + QStringLiteral(".old");
		QFile::remove(strOldFilePath);
		file.rename(strOldFilePath);
	}
}

bool KLatencyTraceLogger::isEnabled()
{
	QMutexLocker locker(&g_latencyTraceMutex);
	initialize();
	return g_bLatencyTraceEnabled;
}

void KLatencyTraceLogger::write(const QString &strSide,
	const QString &strStage,
	const QString &strExtra)
{
	QMutexLocker locker(&g_latencyTraceMutex);
	initialize();
	if (!g_bLatencyTraceEnabled)
		return;

	const QString strFilePath = logFilePath();
	rotateIfNeeded(strFilePath);

	QFile file(strFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
		return;

	QTextStream stream(&file);
	stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
		   << QStringLiteral(" [latency]")
		   << QStringLiteral(" side=") << strSide
		   << QStringLiteral(" stage=") << strStage;
	if (!strExtra.isEmpty())
		stream << QLatin1Char(' ') << strExtra;
	stream << Qt::endl;
}
