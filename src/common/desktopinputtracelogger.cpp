#include "common/desktopinputtracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QQueue>
#include <QtCore/QTextStream>
#include <QtCore/QThread>
#include <QtCore/QWaitCondition>

#include <chrono>
#include <utility>

namespace
{
constexpr qint64 kMaximumFileSize = 5 * 1024 * 1024;
constexpr int kBatchIntervalMs = 100;
constexpr int kBatchSize = 128;
constexpr int kMaximumQueuedLines = 4096;

QString LogFilePath(const QString &strLogDirectory)
{
	return QDir(strLogDirectory).absoluteFilePath(
		QStringLiteral("desktop_input_trace_controlled.log"));
}

void RotateIfNeeded(const QString &strFilePath)
{
	QFile file(strFilePath);
	if (!file.exists() || file.size() < kMaximumFileSize)
		return;

	const QString strOldFilePath = strFilePath + QStringLiteral(".old");
	QFile::remove(strOldFilePath);
	file.rename(strOldFilePath);
}

class KDesktopInputTraceWorker : public QThread
{
public:
	~KDesktopInputTraceWorker() override
	{
		shutdown();
	}

	void configure(bool bEnabled, const QString &strLogDirectory)
	{
		QMutexLocker locker(&m_mutex);
		if (m_bInitialized)
		{
			qWarning() << "Desktop input trace logger was configured after initialization";
			return;
		}
		m_bConfigured = true;
		m_bConfiguredEnabled = bEnabled;
		m_strLogDirectory = QDir::cleanPath(strLogDirectory);
	}

	bool isEnabled()
	{
		QMutexLocker locker(&m_mutex);
		if (m_bInitialized)
			return m_bEnabled && !m_bStopping;
		return m_bConfigured && m_bConfiguredEnabled;
	}

	void enqueue(const QString &strStage, const QString &strExtra)
	{
		initialize();
		const qint64 nMonotonicUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		const quintptr nThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
		QString strLine = QStringLiteral(
			"%1 [desktop_input] monoUs=%2 pid=%3 tid=%4 stage=%5")
			.arg(QDateTime::currentDateTime().toString(
				QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
			.arg(nMonotonicUs)
			.arg(QCoreApplication::applicationPid())
			.arg(nThreadId)
			.arg(strStage);
		if (!strExtra.isEmpty())
			strLine += QLatin1Char(' ') + strExtra;

		QMutexLocker locker(&m_mutex);
		if (!m_bEnabled || m_bStopping)
			return;
		if (m_pendingLines.size() >= kMaximumQueuedLines)
		{
			m_pendingLines.dequeue();
			++m_nDroppedLines;
		}
		m_pendingLines.enqueue(std::move(strLine));
		if (m_pendingLines.size() >= kBatchSize)
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
		QString strLogDirectory;
		{
			QMutexLocker locker(&m_mutex);
			strLogDirectory = m_strLogDirectory;
		}
		if (!QDir().mkpath(strLogDirectory))
		{
			qWarning().noquote() << QStringLiteral(
				"Unable to create desktop input trace directory: %1")
				.arg(strLogDirectory);
			disableAfterWriteFailure();
			return;
		}

		const QString strFilePath = LogFilePath(strLogDirectory);
		for (;;)
		{
			QQueue<QString> pendingLines;
			quint64 nDroppedLines = 0;
			bool bStopping = false;
			{
				QMutexLocker locker(&m_mutex);
				if (m_pendingLines.size() < kBatchSize && !m_bStopping)
					m_waitCondition.wait(&m_mutex, kBatchIntervalMs);
				pendingLines.swap(m_pendingLines);
				nDroppedLines = m_nDroppedLines;
				m_nDroppedLines = 0;
				bStopping = m_bStopping;
			}

			if (pendingLines.isEmpty() && nDroppedLines == 0)
			{
				if (bStopping)
					break;
				continue;
			}

			RotateIfNeeded(strFilePath);
			QFile file(strFilePath);
			if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
			{
				qWarning().noquote() << QStringLiteral(
					"Unable to open desktop input trace log: %1").arg(strFilePath);
				disableAfterWriteFailure();
				return;
			}

			QTextStream stream(&file);
			if (nDroppedLines > 0)
			{
				stream << QDateTime::currentDateTime().toString(
						QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
					<< QStringLiteral(" [desktop_input] stage=logger_overflow dropped=")
					<< nDroppedLines << QLatin1Char('\n');
			}
			while (!pendingLines.isEmpty())
				stream << pendingLines.dequeue() << QLatin1Char('\n');
			stream.flush();
			file.flush();
			file.close();
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
		m_bEnabled = m_bConfigured && m_bConfiguredEnabled;
		if (m_strLogDirectory.isEmpty())
		{
			const QString strBasePath = QCoreApplication::applicationDirPath().isEmpty()
				? QDir::currentPath() : QCoreApplication::applicationDirPath();
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
	QString m_strLogDirectory;
	quint64 m_nDroppedLines = 0;
	bool m_bConfigured = false;
	bool m_bConfiguredEnabled = false;
	bool m_bInitialized = false;
	bool m_bEnabled = false;
	bool m_bStopping = false;
};

KDesktopInputTraceWorker &DesktopInputTraceWorker()
{
	static KDesktopInputTraceWorker worker;
	return worker;
}
}

void KDesktopInputTraceLogger::configure(bool bEnabled, const QString &strLogDirectory)
{
	DesktopInputTraceWorker().configure(bEnabled, strLogDirectory);
}

bool KDesktopInputTraceLogger::isEnabled()
{
	return DesktopInputTraceWorker().isEnabled();
}

void KDesktopInputTraceLogger::write(const QString &strStage, const QString &strExtra)
{
	DesktopInputTraceWorker().enqueue(strStage, strExtra);
}

void KDesktopInputTraceLogger::shutdown()
{
	DesktopInputTraceWorker().shutdown();
}
