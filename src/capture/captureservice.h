#ifndef _WINREMOTECONTROL_CAPTURESERVICE_H_
#define _WINREMOTECONTROL_CAPTURESERVICE_H_

#include "core/media/decodedvideoframe.h"
#include "capture/captureworker.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <mutex>

class QThread;

class KCaptureService : public QObject
{
	Q_OBJECT

public:
	explicit KCaptureService(QObject *pParent = nullptr);
	~KCaptureService() override;

	KCaptureService(const KCaptureService &) = delete;
	KCaptureService &operator=(const KCaptureService &) = delete;

public slots:
	void startCapture();
	void startWebRtcCapture();
	void stopCapture();
	void setStreamConfig(const KStreamConfig &config);
	void setInputTraceState(quint64 nSeq, qint64 nInjectedMs);
	void requestImmediateFrame();

signals:
	void statusChanged(const QString &strStatus);
	void captureError(const QString &strMessage);
	void decodedFrameReady(const KDecodedVideoFrame &frame);
	void webRtcFrameReady(const KVideoFrame &frame);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);

private:
	void startCaptureWithMode(KCaptureWorker::WorkMode mode);
	void clearWorker();
	void enqueueWebRtcFrame(const KVideoFrame &frame);
	void flushLatestWebRtcFrame();
	void clearPendingWebRtcFrame();

	QThread *m_pCaptureThread = nullptr;
	KCaptureWorker *m_pCaptureWorker = nullptr;
	KStreamConfig m_streamConfig;
	quint64 m_nLastInputSeq = 0;
	qint64 m_nLastInputInjectedMs = -1;
	std::mutex m_webRtcFrameMutex;
	KVideoFrame m_pendingWebRtcFrame;
	bool m_bHasPendingWebRtcFrame = false;
	bool m_bWebRtcFrameFlushQueued = false;
	bool m_bAcceptWebRtcFrames = false;
	quint64 m_nDroppedWebRtcSourceFrames = 0;
};

#endif // _WINREMOTECONTROL_CAPTURESERVICE_H_
