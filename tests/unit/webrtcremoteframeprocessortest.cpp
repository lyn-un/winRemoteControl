#include "transport/webrtc/webrtcremoteframeprocessor.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include <api/video/i420_buffer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>

namespace
{
	webrtc::VideoFrame MakeFrame(int nTimestampMs)
	{
		webrtc::scoped_refptr<webrtc::I420Buffer> spBuffer =
			webrtc::I420Buffer::Create(640, 360);
		std::memset(spBuffer->MutableDataY(), nTimestampMs & 0xff,
			static_cast<size_t>(spBuffer->StrideY()) * spBuffer->height());
		std::memset(spBuffer->MutableDataU(), 128,
			static_cast<size_t>(spBuffer->StrideU()) * ((spBuffer->height() + 1) / 2));
		std::memset(spBuffer->MutableDataV(), 128,
			static_cast<size_t>(spBuffer->StrideV()) * ((spBuffer->height() + 1) / 2));
		return webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(spBuffer)
			.set_timestamp_ms(nTimestampMs)
			.build();
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	KWebRtcRemoteFrameProcessor processor;
	std::mutex mutex;
	std::condition_variable condition;
	qint64 nLastTimestampMs = -1;
	bool bUsedWorkerThread = false;
	bool bSharedPixelsSurvivedCopy = false;
	const QThread *pOwnerThread = processor.thread();
	QObject::connect(&processor, &KWebRtcRemoteFrameProcessor::frameReady,
		&processor,
		[&](const KDecodedVideoFrame &frame)
		{
			const KDecodedVideoFrame copiedFrame = frame;
			{
				std::lock_guard<std::mutex> guard(mutex);
				nLastTimestampMs = frame.nTimestampMs;
				bUsedWorkerThread = QThread::currentThread() != pOwnerThread;
				bSharedPixelsSurvivedCopy = frame.hasPixels()
					&& copiedFrame.spBgraBuffer == frame.spBgraBuffer;
			}
			condition.notify_all();
		}, Qt::DirectConnection);

	for (int nIndex = 1; nIndex <= 50; ++nIndex)
		processor.enqueue(MakeFrame(nIndex));

	std::unique_lock<std::mutex> lock(mutex);
	const bool bLatestPresented = condition.wait_for(lock,
		std::chrono::seconds(3), [&]() { return nLastTimestampMs == 50; });
	if (!bLatestPresented)
	{
		std::cerr << "latest coalesced frame was not processed\n";
		return 1;
	}
	if (!bUsedWorkerThread)
	{
		std::cerr << "remote I420 conversion still ran on the Qt owner thread\n";
		return 1;
	}
	if (!bSharedPixelsSurvivedCopy)
	{
		std::cerr << "decoded frame pixels were deep-copied\n";
		return 1;
	}
	return 0;
}
