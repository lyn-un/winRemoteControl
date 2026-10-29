#ifndef _WINREMOTECONTROL_ENCODERDIAGNOSTICS_H_
#define _WINREMOTECONTROL_ENCODERDIAGNOSTICS_H_

#include <QtCore/QString>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Owns only diagnostic snapshots, never codec or frame resources.
class KEncoderDiagnostics final
{
public:

	KEncoderDiagnostics();
	~KEncoderDiagnostics();
	KEncoderDiagnostics(const KEncoderDiagnostics &) = delete;
	KEncoderDiagnostics &operator=(const KEncoderDiagnostics &) = delete;
	void phase(const char *pPhase);
	void begin();
	void finish(bool bSuccess);
	void output(bool bSuccess, quint64 nBytes = 0, bool bKeyFrame = false);
	void noOutput();
	void rates(const QString &strRates);
	void error(const QString &strReason);

private:
	void run();
	std::mutex m_mutex;
	std::condition_variable m_condition;
	bool m_bStopping = false;
	const char *m_pPhase = "initializing";
	std::chrono::steady_clock::time_point m_phaseSince = std::chrono::steady_clock::now();
	quint64 m_nEntered = 0;
	quint64 m_nSubmissions = 0;
	quint64 m_nFinished = 0;
	quint64 m_nOutputs = 0;
	quint64 m_nErrors = 0;
	quint64 m_nNoOutput = 0;
	quint64 m_nOutputBytes = 0;
	quint64 m_nKeyFrames = 0;
	qint64 m_nEncodeTotalUs = 0;
	qint64 m_nEncodeMaxUs = 0;
	qint64 m_nFfmpegTotalUs = 0;
	qint64 m_nFfmpegMaxUs = 0;
	qint64 m_nGpuSubmitCpuTotalUs = 0;
	qint64 m_nGpuSubmitCpuMaxUs = 0;
	std::chrono::steady_clock::time_point m_encodeSince = {};
	QString m_strRates;
	QString m_strLastError;
	QString m_strPendingError;
	std::chrono::steady_clock::time_point m_lastErrorAt = {};
	std::thread m_thread;
};

#endif
