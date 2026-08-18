#ifndef _WINREMOTECONTROL_WEBRTCREMOTEFRAMEPROCESSOR_H_
#define _WINREMOTECONTROL_WEBRTCREMOTEFRAMEPROCESSOR_H_

#include "core/media/decodedvideoframe.h"

#include <QtCore/QObject>

#include <api/video/video_frame.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

class KWebRtcRemoteFrameProcessor final : public QObject
{
	Q_OBJECT

public:
	explicit KWebRtcRemoteFrameProcessor(QObject *pParent = nullptr);
	~KWebRtcRemoteFrameProcessor() override;

	void enqueue(const webrtc::VideoFrame &frame);
	void clear();

signals:
	void frameReady(const KDecodedVideoFrame &frame);
	void frameStatsReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);

private:
	struct KPendingFrame
	{
		webrtc::VideoFrame frame;
		qint64 nCallbackAtMs = -1;
		quint64 nEpoch = 0;
	};

	void processFrames();
	void decodeAndEmit(const webrtc::VideoFrame &frame,
		qint64 nCallbackAtMs,
		quint64 nEpoch);

	std::mutex m_mutex;
	std::condition_variable m_frameCondition;
	std::optional<KPendingFrame> m_pendingFrame;
	std::thread m_processThread;
	bool m_bStopping = false;
	quint64 m_nEpoch = 1;
	quint64 m_nReceivedCallbackFrames = 0;
	quint64 m_nProcessedCallbackFrames = 0;
	quint64 m_nDroppedCallbackFrames = 0;
	quint64 m_nFrameIndex = 0;
};

#endif // _WINREMOTECONTROL_WEBRTCREMOTEFRAMEPROCESSOR_H_
