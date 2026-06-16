#ifndef _WINREMOTECONTROL_CAPTUREWORKER_H_
#define _WINREMOTECONTROL_CAPTUREWORKER_H_

#include "codec/decodedvideoframe.h"
#include "common/streamconfig.h"
#include "transport/webrtc/webrtcvideoframe.h"

#include <QtCore/QObject>
#include <QtCore/QElapsedTimer>
#include <QtCore/QString>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

struct KCaptureFrame;

class KCaptureWorker : public QObject
{
	Q_OBJECT

public:
	enum WorkMode
	{
		LocalPreviewWorkMode,
		WebRtcSourceWorkMode
	};

	explicit KCaptureWorker(WorkMode mode = LocalPreviewWorkMode, QObject *pParent = nullptr);
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
	void webRtcFrameReady(const KWebRtcVideoFrame &frame);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void workFinished();

private:
	KStreamConfig streamConfig() const;
	void inputTraceState(quint64 *pSeq, qint64 *pInjectedMs) const;
	bool waitForNextFrame(qint64 nSleepMs);
	bool shouldTraceImmediateFrameRequest();
	static KStreamConfig normalizeStreamConfig(const KStreamConfig &config);
	static bool convertBgraToI420(const KCaptureFrame &captureFrame,
		const KStreamConfig &config,
		quint64 nLastInputSeq,
		qint64 nLastInputAgeMs,
		KWebRtcVideoFrame *pVideoFrame);
	static bool resizeBgraFrame(const KCaptureFrame &captureFrame,
		int nTargetWidth,
		int nTargetHeight,
		std::vector<unsigned char> *pScaledBgraBuffer);

	WorkMode m_mode = LocalPreviewWorkMode;
	std::atomic_bool m_bRunning = false;
	mutable std::mutex m_configMutex;
	mutable std::mutex m_inputTraceMutex;
	std::mutex m_waitMutex;
	std::condition_variable m_waitCondition;
	KStreamConfig m_streamConfig;
	quint64 m_nLastInputSeq = 0;
	qint64 m_nLastInputInjectedMs = -1;
	bool m_bImmediateFrameRequested = false;
	bool m_bImmediateFrameTracePending = false;
	QElapsedTimer m_immediateFrameTraceTimer;
};

#endif // _WINREMOTECONTROL_CAPTUREWORKER_H_
