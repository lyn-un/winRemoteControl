#include "capture/captureworker.h"

#include <QtCore/QCoreApplication>

#include <iostream>
#include <memory>

namespace
{
	class KFakeCaptureSource : public IKCaptureSource
	{
	public:
		bool initialize(QString *) override
		{
			bInitialized = true;
			return true;
		}

		CaptureResult captureNextFrame(KCaptureFrame *pFrame, QString *pErrorMessage) override
		{
			if (nCaptureCalls < 2)
			{
				++nCaptureCalls;
				pFrame->nWidth = 2;
				pFrame->nHeight = 2;
				pFrame->nFrameIndex = static_cast<quint64>(nCaptureCalls);
				pFrame->vecBgraBuffer.resize(16);
				if (pFirstBuffer == nullptr)
					pFirstBuffer = pFrame->vecBgraBuffer.data();
				else
					bBufferReused = pFirstBuffer == pFrame->vecBgraBuffer.data();
				return CapturedCaptureResult;
			}
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("finished");
			return ErrorCaptureResult;
		}

		void shutdown() override
		{
			bShutdown = true;
		}

		bool bInitialized = false;
		bool bShutdown = false;
		bool bBufferReused = false;
		unsigned char *pFirstBuffer = nullptr;
		int nCaptureCalls = 0;
	};

	class KFakeCaptureFrameSink : public IKCaptureFrameSink
	{
	public:
		bool initialize(QString *) override
		{
			bInitialized = true;
			return true;
		}

		bool processFrame(KCaptureFrame &frame, QString *) override
		{
			++nProcessedFrames;
			nLastFrameIndex = frame.nFrameIndex;
			return true;
		}

		void handleCaptureTimeout() override
		{
			++nTimeouts;
		}

		void shutdown() override
		{
			bShutdown = true;
		}

		void setStreamConfig(const KStreamConfig &config) override
		{
			lastStreamConfig = config;
			++nStreamConfigCount;
		}

		void setInputTraceState(quint64, qint64) override {}

		bool bInitialized = false;
		bool bShutdown = false;
		int nProcessedFrames = 0;
		int nTimeouts = 0;
		quint64 nLastFrameIndex = 0;
		KStreamConfig lastStreamConfig;
		int nStreamConfigCount = 0;
	};

	bool require(bool bCondition, const char *pMessage)
	{
		if (bCondition)
			return true;
		std::cerr << pMessage << std::endl;
		return false;
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	auto upSource = std::make_unique<KFakeCaptureSource>();
	auto upSink = std::make_unique<KFakeCaptureFrameSink>();
	KFakeCaptureSource *pSource = upSource.get();
	KFakeCaptureFrameSink *pSink = upSink.get();
	KCaptureWorker worker(std::move(upSource), std::move(upSink));
	worker.startWork();

	bool bSuccess = true;
	bSuccess &= require(pSource->bInitialized, "capture source was not initialized");
	bSuccess &= require(pSource->bShutdown, "capture source was not shut down");
	bSuccess &= require(pSink->bInitialized, "capture sink was not initialized");
	bSuccess &= require(pSink->bShutdown, "capture sink was not shut down");
	bSuccess &= require(pSink->nProcessedFrames == 2, "captured frames were not routed to sink");
	bSuccess &= require(pSink->nLastFrameIndex == 2, "routed frame metadata changed");
	bSuccess &= require(pSource->bBufferReused,
		"capture loop reuses the BGRA allocation between equal-sized frames");

	KStreamConfig highFpsConfig;
	highFpsConfig.nFps = 144;
	highFpsConfig.nWidth = 1280;
	highFpsConfig.nHeight = 720;
	highFpsConfig.nBitrateKbps = 4000;
	worker.setStreamConfig(highFpsConfig);
	bSuccess &= require(pSink->nStreamConfigCount == 1
		&& pSink->lastStreamConfig.nFps == 144,
		"capture worker forwards stream configs without clamping the frame rate");

	KCaptureFrameSink videoSink(KCaptureFrameSink::RemoteVideoSinkMode);
	std::vector<std::shared_ptr<KI420FrameBuffer>> vecObservedBuffers;
	QObject::connect(&videoSink, &KCaptureFrameSink::videoFrameReady,
		[&](const KVideoFrame &frame) { vecObservedBuffers.push_back(frame.spBuffer); });
	QString strError;

	KStreamConfig sinkFpsConfig;
	sinkFpsConfig.nFps = 144;
	sinkFpsConfig.nWidth = 0;
	sinkFpsConfig.nHeight = 0;
	sinkFpsConfig.nBitrateKbps = 500;
	videoSink.setStreamConfig(sinkFpsConfig);
	bSuccess &= require(videoSink.streamConfig().nFps == 144,
		"frame sink keeps the protocol-maximum frame rate");
	sinkFpsConfig.nFps = 200;
	videoSink.setStreamConfig(sinkFpsConfig);
	bSuccess &= require(videoSink.streamConfig().nFps == 144,
		"frame sink clamps to the shared protocol frame rate limit");
	sinkFpsConfig.nFps = 0;
	videoSink.setStreamConfig(sinkFpsConfig);
	bSuccess &= require(videoSink.streamConfig().nFps == 1,
		"frame sink clamps to the minimum frame rate");

	bSuccess &= require(videoSink.initialize(&strError),
		"remote video sink initializes without local codecs");
	KCaptureFrame videoFrame;
	videoFrame.nWidth = 4;
	videoFrame.nHeight = 4;
	videoFrame.vecBgraBuffer.resize(4 * 4 * 4);
	for (quint64 nFrameIndex = 1; nFrameIndex <= 4; ++nFrameIndex)
	{
		videoFrame.nFrameIndex = nFrameIndex;
		bSuccess &= require(videoSink.processFrame(videoFrame, &strError),
			"I420 conversion succeeds while pool capacity is available");
	}
	bSuccess &= require(vecObservedBuffers.size() == 4,
		"four in-flight frame buffers fill the bounded pool");
	videoFrame.nFrameIndex = 5;
	bSuccess &= require(videoSink.processFrame(videoFrame, &strError)
		&& vecObservedBuffers.size() == 4,
		"pool exhaustion drops a frame without stopping capture");
	KI420FrameBuffer *pFirstBuffer = vecObservedBuffers.front().get();
	vecObservedBuffers.front().reset();
	videoFrame.nFrameIndex = 6;
	bSuccess &= require(videoSink.processFrame(videoFrame, &strError)
		&& vecObservedBuffers.size() == 5
		&& vecObservedBuffers.back().get() == pFirstBuffer,
		"a released I420 buffer is reused instead of allocated again");
	videoSink.shutdown();
	return bSuccess ? 0 : 1;
}
