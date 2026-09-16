#include "transport/webrtc/webrtcremoteframeprocessor.h"
#include "common/latencytracelogger.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>

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
	webrtc::VideoFrame MakeFrame(int nTimestampMs, int nWidth = 640, int nHeight = 360, int nPadding = 0)
	{
		webrtc::scoped_refptr<webrtc::I420Buffer> spBuffer =
			webrtc::I420Buffer::Create(nWidth, nHeight, nWidth + nPadding,
				(nWidth + 1) / 2 + nPadding, (nWidth + 1) / 2 + nPadding);
		std::memset(spBuffer->MutableDataY(), nTimestampMs & 0xff,
			static_cast<size_t>(spBuffer->StrideY()) * spBuffer->height());
		std::memset(spBuffer->MutableDataU(), 128,
			static_cast<size_t>(spBuffer->StrideU()) * ((spBuffer->height() + 1) / 2));
		std::memset(spBuffer->MutableDataV(), 128,
			static_cast<size_t>(spBuffer->StrideV()) * ((spBuffer->height() + 1) / 2));
		webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(spBuffer)
			.set_timestamp_ms(nTimestampMs)
			.build();
		webrtc::VideoFrame::RenderParameters renderParameters;
		renderParameters.use_low_latency_rendering = true;
		frame.set_render_parameters(renderParameters);
		return frame;
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	if (application.arguments().contains(QStringLiteral("--trace-case")))
	{
		QTemporaryDir temporary;
		KLatencyTraceLogger::configure(true, temporary.path(), QStringLiteral("static"));
		KWebRtcRemoteFrameProcessor traceProcessor;
		std::atomic<bool> bPassed{false};
		QObject::connect(&traceProcessor, &KWebRtcRemoteFrameProcessor::frameReady,
			&traceProcessor, [&](const KDecodedVideoFrame &frame)
			{ bPassed.store(frame.hasPixels() && frame.pixelFormat == KDecodedPixelFormat::Bgra); },
			Qt::DirectConnection);
		traceProcessor.enqueue(MakeFrame(1));
		for (int nAttempt = 0; nAttempt < 300 && !bPassed.load(); ++nAttempt)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		KLatencyTraceLogger::shutdown();
		QFile log(temporary.filePath(QStringLiteral("latency_trace_controller.log")));
		if (!log.open(QIODevice::ReadOnly))
			return 1;
		const QByteArray contents = log.readAll();
		return bPassed.load() && contents.contains("actual=bgra reason=latency_trace") ? 0 : 1;
	}
	KWebRtcRemoteFrameProcessor processor;
	std::mutex mutex;
	std::condition_variable condition;
	qint64 nLastTimestampMs = -1;
	bool bUsedWorkerThread = false;
	bool bSharedPixelsSurvivedCopy = false;
	bool bLowLatencyRenderMetadataPreserved = false;
	KDecodedVideoFrame lastFrame;
	const QThread *pOwnerThread = processor.thread();
	QObject::connect(&processor, &KWebRtcRemoteFrameProcessor::frameReady,
		&processor,
		[&](const KDecodedVideoFrame &frame)
		{
			const KDecodedVideoFrame copiedFrame = frame;
			{
				std::lock_guard<std::mutex> guard(mutex);
				nLastTimestampMs = frame.nTimestampMs;
				lastFrame = frame;
				bUsedWorkerThread = QThread::currentThread() != pOwnerThread;
				bSharedPixelsSurvivedCopy = frame.hasPixels()
					&& frame.pixelFormat == KDecodedPixelFormat::I420 && !frame.spBgraBuffer
					&& copiedFrame.spPlaneOwner == frame.spPlaneOwner;
				bLowLatencyRenderMetadataPreserved =
					frame.bWebRtcLowLatencyRender;
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
	if (!bLowLatencyRenderMetadataPreserved)
	{
		std::cerr << "WebRTC low-latency render metadata was not preserved\n";
		return 1;
	}
	const auto oldSession = lastFrame.spRenderSession;
	oldSession->bForceBgra.store(true);
	lock.unlock();
	processor.enqueue(MakeFrame(51));
	lock.lock();
	if (!condition.wait_for(lock, std::chrono::seconds(3), [&]() { return nLastTimestampMs == 51; })
		|| lastFrame.pixelFormat != KDecodedPixelFormat::Bgra || !lastFrame.hasPixels())
	{
		std::cerr << "fallback failed timestamp=" << nLastTimestampMs << " format=" << int(lastFrame.pixelFormat) << '\n';
		return 1;
	}
	lock.unlock();
	processor.clear();
	oldSession->bForceBgra.store(true);
	processor.enqueue(MakeFrame(52));
	lock.lock();
	if (!condition.wait_for(lock, std::chrono::seconds(3), [&]() { return nLastTimestampMs == 52; })
		|| lastFrame.pixelFormat != KDecodedPixelFormat::I420 || oldSession->bActive.load()
		|| lastFrame.spRenderSession == oldSession)
	{
		std::cerr << "generation reset failed timestamp=" << nLastTimestampMs << '\n';
		return 1;
	}
	lock.unlock();
	g_bForceBgraVideoRenderer.store(true);
	processor.enqueue(MakeFrame(53));
	lock.lock();
	if (!condition.wait_for(lock, std::chrono::seconds(3), [&]() { return nLastTimestampMs == 53; })
		|| lastFrame.pixelFormat != KDecodedPixelFormat::Bgra)
	{
		std::cerr << "forced BGRA failed timestamp=" << nLastTimestampMs << '\n';
		return 1;
	}
	g_bForceBgraVideoRenderer.store(false);
	lock.unlock();
	const unsigned char *pOriginalY = nullptr;
	{
		const auto oddFrame = MakeFrame(54, 641, 361, 17);
		pOriginalY = oddFrame.video_frame_buffer()->GetI420()->DataY();
		processor.enqueue(oddFrame);
	}
	lock.lock();
	if (!condition.wait_for(lock, std::chrono::seconds(3), [&]() { return nLastTimestampMs == 54; })
		|| !lastFrame.hasPixels() || lastFrame.pixelFormat != KDecodedPixelFormat::I420
		|| lastFrame.planes[0].pData != pOriginalY || lastFrame.planes[0].nStride != 658
		|| lastFrame.planes[1].nWidth != 321 || lastFrame.planes[1].nHeight != 181
		|| lastFrame.planes[0].pData[0] != 54 || lastFrame.nConversionDoneAtMs != -1)
	{
		std::cerr << "odd padded I420 ownership/metadata failed\n";
		return 1;
	}
	return 0;
}
