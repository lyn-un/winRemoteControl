#include "core/media/latestdecodedframequeue.h"

#include <iostream>
#include <memory>
#include <vector>

namespace
{
KDecodedVideoFrame MakeFrame(quint64 nFrameIndex)
{
	KDecodedVideoFrame frame;
	frame.nWidth = 1;
	frame.nHeight = 1;
	frame.nFrameIndex = nFrameIndex;
	frame.spBgraBuffer = std::make_shared<std::vector<unsigned char>>(4);
	return frame;
}

bool Check(bool bCondition, const char *pDescription)
{
	if (bCondition)
		return true;
	std::cerr << "FAILED: " << pDescription << '\n';
	return false;
}
}

int main()
{
	KLatestDecodedFrameQueue queue;
	const KLatestDecodedFrameEnqueueResult first = queue.enqueue(MakeFrame(1), 10);
	if (!Check(first.bNeedsPresentation && !first.bFrameCoalesced,
			"first frame schedules presentation without coalescing"))
	{
		return 1;
	}

	KDecodedVideoFrame presentedFrame;
	if (!Check(queue.takeLatest(&presentedFrame)
			&& presentedFrame.nFrameIndex == 1,
			"first frame enters presentation"))
	{
		return 1;
	}

	const KLatestDecodedFrameEnqueueResult second = queue.enqueue(MakeFrame(2), 20);
	const KLatestDecodedFrameEnqueueResult third = queue.enqueue(MakeFrame(3), 30);
	if (!Check(!second.bNeedsPresentation && !second.bFrameCoalesced
			&& !third.bNeedsPresentation && third.bFrameCoalesced,
			"frames arriving during presentation share the queued callback"))
	{
		return 1;
	}
	queue.recordPresented();
	if (!Check(queue.completePresentation(),
			"pending latest frame schedules the next presentation"))
	{
		return 1;
	}
	if (!Check(queue.takeLatest(&presentedFrame)
			&& presentedFrame.nFrameIndex == 3
			&& presentedFrame.nRenderEnqueuedAtMs == 30,
			"the newest frame survives while the superseded frame is discarded"))
	{
		return 1;
	}
	const KLatestDecodedFrameQueueStats finalStats = queue.recordPresented();
	if (!Check(!queue.completePresentation()
			&& finalStats.nReceivedFrames == 3
			&& finalStats.nPresentedFrames == 2
			&& finalStats.nCoalescedFrames == 1,
			"queue counters distinguish received, presented, and coalesced frames"))
	{
		return 1;
	}
	return 0;
}
