#ifndef _WINREMOTECONTROL_CORE_MEDIA_LATESTDECODEDFRAMEQUEUE_H_
#define _WINREMOTECONTROL_CORE_MEDIA_LATESTDECODEDFRAMEQUEUE_H_

#include "core/media/decodedvideoframe.h"

#include <mutex>

struct KLatestDecodedFrameQueueStats
{
	quint64 nReceivedFrames = 0;
	quint64 nPresentedFrames = 0;
	quint64 nCoalescedFrames = 0;
};

struct KLatestDecodedFrameEnqueueResult
{
	KLatestDecodedFrameQueueStats stats;
	bool bNeedsPresentation = false;
	bool bFrameCoalesced = false;
};

class KLatestDecodedFrameQueue
{
public:
	KLatestDecodedFrameQueue() = default;

	KLatestDecodedFrameQueue(const KLatestDecodedFrameQueue &) = delete;
	KLatestDecodedFrameQueue &operator=(const KLatestDecodedFrameQueue &) = delete;

	KLatestDecodedFrameEnqueueResult enqueue(
		const KDecodedVideoFrame &frame,
		qint64 nEnqueuedAtMs);
	bool takeLatest(KDecodedVideoFrame *pFrame);
	KLatestDecodedFrameQueueStats recordPresented();
	bool completePresentation();
	KLatestDecodedFrameQueueStats clear();

private:
	std::mutex m_mutex;
	KDecodedVideoFrame m_latestFrame;
	KLatestDecodedFrameQueueStats m_stats;
	bool m_bHasPendingFrame = false;
	bool m_bPresentationQueued = false;
};

#endif // _WINREMOTECONTROL_CORE_MEDIA_LATESTDECODEDFRAMEQUEUE_H_
