#ifndef _WINREMOTECONTROL_WEBRTCREMOTEFRAMEPROCESSOR_H_
#define _WINREMOTECONTROL_WEBRTCREMOTEFRAMEPROCESSOR_H_

#include "core/media/decodedvideoframe.h"

#include <QtCore/QObject>

#include <api/video/video_frame.h>

#include <mutex>
#include <optional>

class KWebRtcRemoteFrameProcessor final : public QObject
{
	Q_OBJECT

public:
	explicit KWebRtcRemoteFrameProcessor(QObject *pParent = nullptr);

	void enqueue(const webrtc::VideoFrame &frame);
	void clear();

signals:
	void frameReady(const KDecodedVideoFrame &frame);
	void frameStatsReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);

private:
	void processLatest();
	void decodeAndEmit(const webrtc::VideoFrame &frame);

	std::mutex m_mutex;
	std::optional<webrtc::VideoFrame> m_pendingFrame;
	bool m_bHasPendingFrame = false;
	bool m_bProcessQueued = false;
	quint64 m_nReceivedCallbackFrames = 0;
	quint64 m_nProcessedCallbackFrames = 0;
	quint64 m_nDroppedCallbackFrames = 0;
	quint64 m_nFrameIndex = 0;
};

#endif // _WINREMOTECONTROL_WEBRTCREMOTEFRAMEPROCESSOR_H_
