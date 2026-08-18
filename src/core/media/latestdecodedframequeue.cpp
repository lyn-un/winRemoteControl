#include "core/media/latestdecodedframequeue.h"

#include <utility>

KLatestDecodedFrameEnqueueResult KLatestDecodedFrameQueue::enqueue(
	const KDecodedVideoFrame &frame,
	qint64 nEnqueuedAtMs)
{
	std::lock_guard<std::mutex> guard(m_mutex);
	KLatestDecodedFrameEnqueueResult result;
	++m_stats.nReceivedFrames;
	if (m_bHasPendingFrame)
	{
		++m_stats.nCoalescedFrames;
		result.bFrameCoalesced = true;
	}
	m_latestFrame = frame;
	m_latestFrame.nRenderEnqueuedAtMs = nEnqueuedAtMs;
	m_bHasPendingFrame = true;
	if (!m_bPresentationQueued)
	{
		m_bPresentationQueued = true;
		result.bNeedsPresentation = true;
	}
	result.stats = m_stats;
	return result;
}

bool KLatestDecodedFrameQueue::takeLatest(KDecodedVideoFrame *pFrame)
{
	if (pFrame == nullptr)
		return false;

	std::lock_guard<std::mutex> guard(m_mutex);
	if (!m_bHasPendingFrame)
	{
		m_bPresentationQueued = false;
		return false;
	}
	*pFrame = std::move(m_latestFrame);
	m_bHasPendingFrame = false;
	return true;
}

KLatestDecodedFrameQueueStats KLatestDecodedFrameQueue::recordPresented()
{
	std::lock_guard<std::mutex> guard(m_mutex);
	++m_stats.nPresentedFrames;
	return m_stats;
}

bool KLatestDecodedFrameQueue::completePresentation()
{
	std::lock_guard<std::mutex> guard(m_mutex);
	if (m_bHasPendingFrame)
		return true;
	m_bPresentationQueued = false;
	return false;
}

KLatestDecodedFrameQueueStats KLatestDecodedFrameQueue::clear()
{
	std::lock_guard<std::mutex> guard(m_mutex);
	const KLatestDecodedFrameQueueStats stats = m_stats;
	m_latestFrame = KDecodedVideoFrame();
	m_stats = KLatestDecodedFrameQueueStats();
	m_bHasPendingFrame = false;
	m_bPresentationQueued = false;
	return stats;
}
