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
			if (nCaptureCalls++ == 0)
			{
				pFrame->nWidth = 2;
				pFrame->nHeight = 2;
				pFrame->nFrameIndex = 1;
				pFrame->vecBgraBuffer.resize(16);
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

		bool processFrame(KCaptureFrame frame, QString *) override
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

		void setStreamConfig(const KStreamConfig &) override {}
		void setInputTraceState(quint64, qint64) override {}

		bool bInitialized = false;
		bool bShutdown = false;
		int nProcessedFrames = 0;
		int nTimeouts = 0;
		quint64 nLastFrameIndex = 0;
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
	bSuccess &= require(pSink->nProcessedFrames == 1, "captured frame was not routed to sink");
	bSuccess &= require(pSink->nLastFrameIndex == 1, "routed frame metadata changed");
	return bSuccess ? 0 : 1;
}
