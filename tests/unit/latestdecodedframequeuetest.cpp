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
	queue.clear();
	KDecodedVideoFrame i420;
	i420.pixelFormat = KDecodedPixelFormat::I420;
	i420.nWidth = 3;
	i420.nHeight = 3;
	auto pixels = std::make_shared<std::vector<unsigned char>>(32, 128);
	std::weak_ptr<std::vector<unsigned char>> weakPixels = pixels;
	i420.spPlaneOwner = pixels;
	i420.planes = {{{pixels->data(), 3, 3, 4, 12},
		{pixels->data() + 12, 2, 2, 4, 8}, {pixels->data() + 20, 2, 2, 4, 8}}};
	i420.spRenderSession = std::make_shared<KVideoRenderSession>();
	if (!Check(i420.hasPixels(), "odd I420 with padded strides is valid")) return 1;
	auto malformed = i420;
	malformed.planes[1].nStride = -1;
	if (!Check(!malformed.hasPixels(), "negative stride rejected")) return 1;
	malformed = i420;
	malformed.planes[2].nHeight = 3;
	if (!Check(!malformed.hasPixels(), "incorrect chroma dimensions rejected")) return 1;
	malformed = i420;
	malformed.spPlaneOwner.reset();
	if (!Check(!malformed.hasPixels(), "unowned I420 pointer rejected")) return 1;
	malformed = {};
	queue.enqueue(i420, 40);
	pixels.reset();
	i420 = {};
	if (!Check(!weakPixels.expired(), "queue retains I420 owner")) return 1;
	queue.enqueue(MakeFrame(5), 50);
	if (!Check(weakPixels.expired(), "coalescing releases superseded I420 owner")) return 1;
	queue.clear();
	auto invalid = MakeFrame(6);
	invalid.spBgraBuffer->resize(3);
	if (!Check(!invalid.hasPixels(), "undersized BGRA rejected")) return 1;
	invalid = MakeFrame(7);
	invalid.spRenderSession = std::make_shared<KVideoRenderSession>();
	invalid.spRenderSession->bActive.store(false);
	if (!Check(!invalid.hasPixels(), "retired session frame rejected")) return 1;
	if (!Check(!queue.enqueue(invalid, 70).bNeedsPresentation,
		"late retired frame cannot schedule presentation after clear")) return 1;
	invalid = MakeFrame(8);
	invalid.pixelFormat = static_cast<KDecodedPixelFormat>(99);
	if (!Check(!invalid.hasPixels(), "unknown pixel format rejected")) return 1;
	auto pending = MakeFrame(9);
	pending.spRenderSession = std::make_shared<KVideoRenderSession>();
	queue.enqueue(pending, 90);
	pending.spRenderSession->bActive.store(false);
	if (!Check(!queue.takeLatest(&presentedFrame), "retired queued frame cannot be consumed")) return 1;
	if (!Check(queue.enqueue(MakeFrame(10), 100).bNeedsPresentation,
		"fresh frame schedules after retired queued frame is dropped")) return 1;
	return 0;
}
