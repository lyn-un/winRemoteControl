#include "transport/webrtc/encoderdiagnostics.h"
#include "common/sessiontracelogger.h"
#include <utility>
#include <cstring>
#include <algorithm>

KEncoderDiagnostics::KEncoderDiagnostics()
	: m_thread([this]() { run(); })
{
}

KEncoderDiagnostics::~KEncoderDiagnostics()
{
	{
		std::lock_guard lock(m_mutex);
		m_bStopping = true;
	}
	m_condition.notify_one();
	m_thread.join();
}

void KEncoderDiagnostics::phase(const char *pPhase)
{
	std::lock_guard lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	if (std::strcmp(m_pPhase, "ffmpeg_encode") == 0)
	{
		const auto nUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_phaseSince).count();
		m_nFfmpegTotalUs += nUs;
		m_nFfmpegMaxUs = std::max(m_nFfmpegMaxUs, static_cast<qint64>(nUs));
	}
	else if (std::strcmp(m_pPhase, "gpu_convert_submit") == 0)
	{
		const auto nUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_phaseSince).count();
		m_nGpuSubmitCpuTotalUs += nUs;
		m_nGpuSubmitCpuMaxUs = std::max(m_nGpuSubmitCpuMaxUs, static_cast<qint64>(nUs));
	}
	m_pPhase = pPhase;
	if (std::strcmp(pPhase, "ffmpeg_encode") == 0 || std::strcmp(pPhase, "gpu_convert_submit") == 0)
		++m_nSubmissions;
	m_phaseSince = std::chrono::steady_clock::now();
}

void KEncoderDiagnostics::begin()
{
	std::lock_guard lock(m_mutex);
	++m_nEntered;
	m_pPhase = "prepare";
	m_phaseSince = std::chrono::steady_clock::now();
	m_encodeSince = m_phaseSince;
}

void KEncoderDiagnostics::finish(bool bSuccess)
{
	std::lock_guard lock(m_mutex);
	++m_nFinished;
	if (!bSuccess)
		++m_nErrors;
	const auto now = std::chrono::steady_clock::now();
	const auto nUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_encodeSince).count();
	m_nEncodeTotalUs += nUs;
	m_nEncodeMaxUs = std::max(m_nEncodeMaxUs, static_cast<qint64>(nUs));
	if (std::strcmp(m_pPhase, "ffmpeg_encode") == 0)
	{
		const auto nFfmpegUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_phaseSince).count();
		m_nFfmpegTotalUs += nFfmpegUs;
		m_nFfmpegMaxUs = std::max(m_nFfmpegMaxUs, static_cast<qint64>(nFfmpegUs));
	}
	else if (std::strcmp(m_pPhase, "gpu_convert_submit") == 0)
	{
		const auto nUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_phaseSince).count();
		m_nGpuSubmitCpuTotalUs += nUs;
		m_nGpuSubmitCpuMaxUs = std::max(m_nGpuSubmitCpuMaxUs, static_cast<qint64>(nUs));
	}
	m_pPhase = "idle";
	m_phaseSince = std::chrono::steady_clock::now();
}

void KEncoderDiagnostics::output(bool bSuccess, quint64 nBytes, bool bKeyFrame)
{
	std::lock_guard lock(m_mutex);
	if (bSuccess)
	{
		++m_nOutputs;
		m_nOutputBytes += nBytes;
		m_nKeyFrames += bKeyFrame ? 1 : 0;
	}
}

void KEncoderDiagnostics::noOutput()
{
	std::lock_guard lock(m_mutex);
	++m_nNoOutput;
}

void KEncoderDiagnostics::rates(const QString &strRates)
{
	std::lock_guard lock(m_mutex);
	m_strRates = strRates;
}

void KEncoderDiagnostics::error(const QString &strReason)
{
	std::lock_guard lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	// One bounded pending event; repeated failures remain visible in cumulative counters.
	if (m_strLastError.isEmpty() || now - m_lastErrorAt >= std::chrono::seconds(1))
	{
		m_strPendingError = strReason;
		m_lastErrorAt = now;
		m_condition.notify_one();
	}
	m_strLastError = strReason;
}

void KEncoderDiagnostics::run()
{
	const QString strId = QString::number(reinterpret_cast<quintptr>(this), 16);
	auto lastSummary = std::chrono::steady_clock::now();
	quint64 nLastBytes = 0;
	auto nextSummary = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	std::unique_lock lock(m_mutex);
	for (;;)
	{
		m_condition.wait_until(lock, nextSummary, [this]()
			{ return m_bStopping || !m_strPendingError.isEmpty(); });
		const auto now = std::chrono::steady_clock::now();
		const bool bSummary = m_bStopping || now >= nextSummary;
		const bool bStopping = m_bStopping;
		const QString strError = std::exchange(m_strPendingError, QString());
		QString strSummary;
		if (bSummary)
		{
			const auto nPhaseMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_phaseSince).count();
			strSummary = QStringLiteral("instance=%1 phase=%2 phaseMs=%3 entered=%4 finished=%5 outputs=%6 errors=%7 noOutput=%8 final=%9 submissions=%10 %11 lastError=%12")
				.arg(strId).arg(QString::fromLatin1(m_pPhase)).arg(nPhaseMs)
				.arg(m_nEntered).arg(m_nFinished).arg(m_nOutputs).arg(m_nErrors).arg(m_nNoOutput)
				.arg(bStopping ? 1 : 0).arg(m_nSubmissions).arg(m_strRates, m_strLastError);
			const auto nWindowUs = std::chrono::duration_cast<std::chrono::microseconds>(now - lastSummary).count();
			const double fOutputBps = nWindowUs > 0
				? static_cast<double>(m_nOutputBytes - nLastBytes) * 8000000.0 / nWindowUs : 0.0;
			strSummary += QStringLiteral(" outputBytes=%1 keyFrames=%2 windowUs=%3 measuredOutputBps=%4 encodeTotalUs=%5 encodeMaxUs=%6 ffmpegTotalUs=%7 ffmpegMaxUs=%8")
				.arg(m_nOutputBytes).arg(m_nKeyFrames).arg(nWindowUs).arg(fOutputBps, 0, 'f', 0)
				.arg(m_nEncodeTotalUs).arg(m_nEncodeMaxUs).arg(m_nFfmpegTotalUs).arg(m_nFfmpegMaxUs);
			strSummary += QStringLiteral(" gpuSubmitCpuTotalUs=%1 gpuSubmitCpuMaxUs=%2")
				.arg(m_nGpuSubmitCpuTotalUs).arg(m_nGpuSubmitCpuMaxUs);
			lastSummary = now;
			nLastBytes = m_nOutputBytes;
			nextSummary = now + std::chrono::seconds(1);
		}
		lock.unlock();
		if (!strError.isEmpty())
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_error"),
				QStringLiteral("h264"), -1, QStringLiteral("instance=%1 %2").arg(strId, strError));
		if (bSummary)
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_watch"),
				QStringLiteral("h264"), -1, strSummary);
		if (bStopping)
			return;
		lock.lock();
	}
}
