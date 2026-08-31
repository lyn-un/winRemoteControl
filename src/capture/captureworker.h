#ifndef _WINREMOTECONTROL_CAPTUREWORKER_H_
#define _WINREMOTECONTROL_CAPTUREWORKER_H_

#include "capture/captureframesink.h"
#include "core/media/capturesource.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

class KCaptureWorker : public QObject
{
	Q_OBJECT

public:
	enum WorkMode
	{
		LocalPreviewWorkMode,
		RemoteVideoWorkMode
	};

	explicit KCaptureWorker(WorkMode mode = LocalPreviewWorkMode, QObject *pParent = nullptr);
	KCaptureWorker(std::unique_ptr<IKCaptureSource> upSource,
		std::unique_ptr<IKCaptureFrameSink> upSink,
		QObject *pParent = nullptr);
	~KCaptureWorker() override;

	KCaptureWorker(const KCaptureWorker &) = delete;
	KCaptureWorker &operator=(const KCaptureWorker &) = delete;

public slots:
	void startWork();
	void stopWork();
	void setStreamConfig(const KStreamConfig &config);
	void setInputTraceState(quint64 nSeq, qint64 nInjectedMs);
	void requestImmediateFrame();

signals:
	void statusChanged(const QString &strStatus);
	void captureError(const QString &strMessage);
	void decodedFrameReady(const KDecodedVideoFrame &frame);
	void videoFrameReady(const KVideoFrame &frame);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void workFinished();

private:
	bool waitForNextFrame(qint64 nWaitUs);
	bool shouldTraceImmediateFrameRequest();

	std::unique_ptr<IKCaptureSource> m_upSource;
	std::unique_ptr<IKCaptureFrameSink> m_upSink;
	std::atomic_bool m_bRunning = false;
	std::mutex m_waitMutex;
	std::condition_variable m_waitCondition;
	// Capture cadence is an absolute monotonic deadline in microseconds so that
	// high frame rates (90-144 FPS) do not accumulate integer-millisecond drift.
	// Guarded by m_waitMutex.
	qint64 m_nFrameIntervalUs = 1000000 / 30;
	qint64 m_nNextFrameDeadlineUs = 0;
	bool m_bCadenceResetPending = true;
	QElapsedTimer m_cadenceClock;
	bool m_bRemoteVideo = false;
	bool m_bImmediateFrameRequested = false;
	bool m_bImmediateFrameTracePending = false;
	QElapsedTimer m_immediateFrameTraceTimer;
};

#endif // _WINREMOTECONTROL_CAPTUREWORKER_H_
