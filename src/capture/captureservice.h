#ifndef _WINREMOTECONTROL_CAPTURESERVICE_H_
#define _WINREMOTECONTROL_CAPTURESERVICE_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/capturecontroller.h"
#include "capture/captureworker.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <mutex>

class QThread;

class KCaptureService : public KCaptureController
{
	Q_OBJECT

public:
	explicit KCaptureService(QObject *pParent = nullptr);
	~KCaptureService() override;

	KCaptureService(const KCaptureService &) = delete;
	KCaptureService &operator=(const KCaptureService &) = delete;

public slots:
	void startCapture() override;
	void startWebRtcCapture(quint64 nGeneration);
	void stopCapture() override;
	void requestStopCapture(quint64 nGeneration);
	void setStreamConfig(const KStreamConfig &config);
	void setInputTraceState(quint64 nSeq, qint64 nInjectedMs);
	void requestImmediateFrame();

signals:
	void webRtcFrameReady(quint64 nGeneration, const KVideoFrame &frame);
	void captureShutdownFinished(quint64 nGeneration);

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
	quint64 m_nGeneration = 0;
	quint64 m_nStoppingGeneration = 0;
	bool m_bStartPending = false;
	KCaptureWorker::WorkMode m_pendingMode = KCaptureWorker::LocalPreviewWorkMode;
	quint64 m_nDroppedWebRtcSourceFrames = 0;
};

#endif // _WINREMOTECONTROL_CAPTURESERVICE_H_
