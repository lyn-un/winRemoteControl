#include "common/latencytracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QQueue>
#include <QtCore/QThread>
#include <QtCore/QTextStream>
#include <QtCore/QWaitCondition>

#include <chrono>
#include <utility>

namespace
{
	constexpr qint64 kLatencyTraceMaxFileSize = 5 * 1024 * 1024;
	constexpr int kLatencyTraceBatchIntervalMs = 100;
	constexpr int kLatencyTraceBatchSize = 256;
	constexpr int kLatencyTraceMaxQueuedLines = 8192;
	constexpr char kLatencyTraceEnvName[] = "WRC_LATENCY_TRACE";

	static QString logFilePath(const QString &strLogDirectory,
		const QString &strProcessSide)
	{
		return QDir(strLogDirectory).absoluteFilePath(
			QStringLiteral("latency_trace_%1.log").arg(strProcessSide));
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

	class KLatencyTraceWorker : public QThread
	{
	public:
		KLatencyTraceWorker() = default;

		~KLatencyTraceWorker() override
		{
			shutdown();
		}

		bool isEnabled()
		{
			initialize();
			QMutexLocker locker(&m_mutex);
			return m_bEnabled && !m_bStopping;
		}

		void configure(bool bEnabled, const QString &strLogDirectory)
		{
			QMutexLocker locker(&m_mutex);
			if (m_bInitialized)
			{
				qWarning() << "Latency trace logger was configured after initialization";
				return;
			}
			m_bConfigured = true;
			m_bConfiguredEnabled = bEnabled;
			m_strLogDirectory = QDir::cleanPath(strLogDirectory);
		}

		void enqueue(const QString &strSide, const QString &strStage, const QString &strExtra)
		{
			initialize();
			{
				QMutexLocker locker(&m_mutex);
				if (!m_bEnabled || m_bStopping)
					return;
				if (m_strProcessSide.isEmpty()
					&& (strSide == QStringLiteral("controller") || strSide == QStringLiteral("controlled")))
				{
					m_strProcessSide = strSide;
					m_waitCondition.wakeOne();
				}
			}

			const qint64 nMonotonicUs = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			const QString strWallTime = QDateTime::currentDateTime().toString(
				QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
			const quintptr nThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());

			QString strLine = QStringLiteral("%1 [latency] monoUs=%2 pid=%3 tid=%4 side=%5 stage=%6")
				.arg(strWallTime)
				.arg(nMonotonicUs)
				.arg(QCoreApplication::applicationPid())
				.arg(nThreadId)
				.arg(strSide)
				.arg(strStage);
			if (!strExtra.isEmpty())
				strLine += QLatin1Char(' ') + strExtra;

			QMutexLocker locker(&m_mutex);
			if (!m_bEnabled || m_bStopping)
				return;
			if (m_pendingLines.size() >= kLatencyTraceMaxQueuedLines)
			{
				m_pendingLines.dequeue();
				++m_nDroppedLines;
			}
			m_pendingLines.enqueue(std::move(strLine));
			if (m_pendingLines.size() >= kLatencyTraceBatchSize)
				m_waitCondition.wakeOne();
		}

		void shutdown()
		{
			bool bShouldWait = false;
			{
				QMutexLocker locker(&m_mutex);
				if (!m_bInitialized)
					return;
				if (m_bEnabled && !m_bStopping)
				{
					m_bStopping = true;
					m_waitCondition.wakeOne();
				}
				bShouldWait = isRunning();
			}
			if (bShouldWait)
				wait();
		}

	protected:
		void run() override
		{
			QString strProcessSide;
			QString strLogDirectory;
			{
				QMutexLocker locker(&m_mutex);
				while (m_strProcessSide.isEmpty() && !m_bStopping)
					m_waitCondition.wait(&m_mutex);
				strProcessSide = m_strProcessSide.isEmpty()
					? QStringLiteral("unknown")
					: m_strProcessSide;
				strLogDirectory = m_strLogDirectory;
			}

			if (!QDir().mkpath(strLogDirectory))
			{
				qWarning().noquote() << QStringLiteral("Unable to create trace log directory: %1")
					.arg(strLogDirectory);
				disableAfterWriteFailure();
				return;
			}
			const QString strFilePath = logFilePath(strLogDirectory, strProcessSide);
			rotateIfNeeded(strFilePath);

			QFile file(strFilePath);
			if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
			{
				qWarning().noquote() << QStringLiteral("Unable to open latency trace log: %1")
					.arg(strFilePath);
				disableAfterWriteFailure();
				return;
			}

			QTextStream stream(&file);
			for (;;)
			{
				QQueue<QString> pendingLines;
				quint64 nDroppedLines = 0;
				bool bStopping = false;
				{
					QMutexLocker locker(&m_mutex);
					if (m_pendingLines.size() < kLatencyTraceBatchSize && !m_bStopping)
						m_waitCondition.wait(&m_mutex, kLatencyTraceBatchIntervalMs);
					pendingLines.swap(m_pendingLines);
					nDroppedLines = m_nDroppedLines;
					m_nDroppedLines = 0;
					bStopping = m_bStopping;
				}

				const bool bHasOutput = nDroppedLines > 0 || !pendingLines.isEmpty();
				if (nDroppedLines > 0)
				{
					stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
						   << QStringLiteral(" [latency] side=app stage=logger_overflow dropped=")
						   << nDroppedLines << QLatin1Char('\n');
				}
				while (!pendingLines.isEmpty())
					stream << pendingLines.dequeue() << QLatin1Char('\n');
				if (bHasOutput)
					file.flush();

				if (bStopping)
					break;
			}
		}

	private:
		void initialize()
		{
			QMutexLocker locker(&m_mutex);
			if (m_bInitialized)
				return;
			m_bInitialized = true;
			m_bEnabled = m_bConfigured
				? m_bConfiguredEnabled
				: qEnvironmentVariableIsSet(kLatencyTraceEnvName)
					&& qEnvironmentVariable(kLatencyTraceEnvName) != QStringLiteral("0");
			if (m_strLogDirectory.isEmpty())
			{
				const QString strBasePath = QCoreApplication::applicationDirPath().isEmpty()
					? QDir::currentPath()
					: QCoreApplication::applicationDirPath();
				m_strLogDirectory = QDir(strBasePath).absoluteFilePath(QStringLiteral("logs"));
			}
			if (m_bEnabled)
				start();
		}

		void disableAfterWriteFailure()
		{
			QMutexLocker locker(&m_mutex);
			m_bEnabled = false;
			m_pendingLines.clear();
		}

		QMutex m_mutex;
		QWaitCondition m_waitCondition;
		QQueue<QString> m_pendingLines;
		QString m_strProcessSide;
		QString m_strLogDirectory;
		quint64 m_nDroppedLines = 0;
		bool m_bConfigured = false;
		bool m_bConfiguredEnabled = false;
		bool m_bInitialized = false;
		bool m_bEnabled = false;
		bool m_bStopping = false;
	};

	static KLatencyTraceWorker &latencyTraceWorker()
	{
		static KLatencyTraceWorker worker;
		return worker;
	}
}

void KLatencyTraceLogger::configure(bool bEnabled, const QString &strLogDirectory)
{
	latencyTraceWorker().configure(bEnabled, strLogDirectory);
}

bool KLatencyTraceLogger::isEnabled()
{
	return latencyTraceWorker().isEnabled();
}

void KLatencyTraceLogger::write(const QString &strSide,
	const QString &strStage,
	const QString &strExtra)
{
	latencyTraceWorker().enqueue(strSide, strStage, strExtra);
}

void KLatencyTraceLogger::shutdown()
{
	latencyTraceWorker().shutdown();
}
