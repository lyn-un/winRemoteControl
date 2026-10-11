#ifndef _WINREMOTECONTROL_VIDEOSTAGEDIAGNOSTICS_H_
#define _WINREMOTECONTROL_VIDEOSTAGEDIAGNOSTICS_H_

#include "common/sessiontracelogger.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Bounded snapshots only: no pixels, input events, or frame ownership.
class KVideoStageDiagnostics final
{
public:

	explicit KVideoStageDiagnostics(const QString &strStage)
		: m_strStage(strStage)
		, m_thread([this]() { run(); })
	{
	}
	~KVideoStageDiagnostics()
	{
		{
			std::lock_guard lock(m_mutex);
			m_bStopping = true;
		}
		m_condition.notify_one();
		m_thread.join();
	}
	KVideoStageDiagnostics(const KVideoStageDiagnostics &) = delete;
	KVideoStageDiagnostics &operator=(const KVideoStageDiagnostics &) = delete;

	void record(qint64 nDurationUs, bool bSuccess, bool bSkipped,
		int nWidth = 0, int nHeight = 0)
	{
		std::lock_guard lock(m_mutex);
		++m_nCompleted;
		m_nSuccess += bSuccess ? 1 : 0;
		m_nSkipped += bSkipped ? 1 : 0;
		m_nErrors += !bSuccess && !bSkipped ? 1 : 0;
		m_nTotalUs += nDurationUs;
		m_nMaxUs = std::max(m_nMaxUs, nDurationUs);
		if (bSuccess)
		{
			m_nWidth = nWidth;
			m_nHeight = nHeight;
		}
	}

private:
	void run()
	{
		const auto started = std::chrono::steady_clock::now();
		const QString strId = QString::number(reinterpret_cast<quintptr>(this), 16);
		std::unique_lock lock(m_mutex);
		for (;;)
		{
			m_condition.wait_for(lock, std::chrono::seconds(1), [this]() { return m_bStopping; });
			const bool bStopping = m_bStopping;
			const auto nElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			const QString strSummary = QStringLiteral("instance=%1 elapsedMs=%2 completed=%3 success=%4 skipped=%5 errors=%6 totalUs=%7 maxUs=%8 width=%9 height=%10 final=%11")
				.arg(strId).arg(nElapsedMs).arg(m_nCompleted).arg(m_nSuccess).arg(m_nSkipped)
				.arg(m_nErrors).arg(m_nTotalUs).arg(m_nMaxUs).arg(m_nWidth).arg(m_nHeight).arg(bStopping ? 1 : 0);
			lock.unlock();
			KSessionTraceLogger::write(QStringLiteral("controlled"), m_strStage,
				QStringLiteral("video_pipeline"), -1, strSummary);
			if (bStopping)
				return;
			lock.lock();
		}
	}

	QString m_strStage;
	std::mutex m_mutex;
	std::condition_variable m_condition;
	bool m_bStopping = false;
	quint64 m_nCompleted = 0;
	quint64 m_nSuccess = 0;
	quint64 m_nSkipped = 0;
	quint64 m_nErrors = 0;
	qint64 m_nTotalUs = 0;
	qint64 m_nMaxUs = 0;
	int m_nWidth = 0;
	int m_nHeight = 0;
	std::thread m_thread;
};

#endif
