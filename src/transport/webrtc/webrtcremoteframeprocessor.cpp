#include "transport/webrtc/webrtcremoteframeprocessor.h"

#include "common/framewatermark.h"
#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"

#include <api/video/i420_buffer.h>

#include <libyuv.h>

#include <utility>
#include <chrono>

namespace
{
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr quint64 kCallbackFrameCoalesceTraceInterval = 30;

	qint64 SteadyNowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

KWebRtcRemoteFrameProcessor::KWebRtcRemoteFrameProcessor(QObject *pParent)
	: QObject(pParent)
{
	m_spRenderSession->nGeneration = m_nEpoch;
	m_processThread = std::thread(
		&KWebRtcRemoteFrameProcessor::processFrames, this);
}

KWebRtcRemoteFrameProcessor::~KWebRtcRemoteFrameProcessor()
{
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		m_bStopping = true;
		m_spRenderSession->bActive.store(false);
		m_pendingFrame.reset();
	}
	m_frameCondition.notify_all();
	if (m_processThread.joinable())
		m_processThread.join();
}

void KWebRtcRemoteFrameProcessor::enqueue(const webrtc::VideoFrame &frame)
{
	const qint64 nCallbackAtMs = SteadyNowMs();
	bool bFrameCoalesced = false;
	quint64 nReceivedFrames = 0;
	quint64 nProcessedFrames = 0;
	quint64 nDroppedFrames = 0;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		++m_nReceivedCallbackFrames;
		if (m_pendingFrame.has_value())
		{
			++m_nDroppedCallbackFrames;
			bFrameCoalesced = true;
		}
		m_pendingFrame = KPendingFrame{ frame, nCallbackAtMs, m_nEpoch };
		nReceivedFrames = m_nReceivedCallbackFrames;
		nProcessedFrames = m_nProcessedCallbackFrames;
		nDroppedFrames = m_nDroppedCallbackFrames;
	}
	m_frameCondition.notify_one();

	if (bFrameCoalesced
		&& KLatencyTraceLogger::isEnabled()
		&& (nDroppedFrames == 1
			|| nDroppedFrames % kCallbackFrameCoalesceTraceInterval == 0))
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("remote_callback_frame_coalesced"),
			QStringLiteral("stage=conversion_queue receivedTotal=%1 processedTotal=%2 coalescedTotal=%3 latestTimestampMs=%4")
				.arg(nReceivedFrames)
				.arg(nProcessedFrames)
				.arg(nDroppedFrames)
				.arg(frame.timestamp_us() / 1000));
	}

}

void KWebRtcRemoteFrameProcessor::clear()
{
	quint64 nReceivedFrames = 0;
	quint64 nProcessedFrames = 0;
	quint64 nDroppedFrames = 0;
	bool bPendingFrameDiscarded = false;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		nReceivedFrames = m_nReceivedCallbackFrames;
		nProcessedFrames = m_nProcessedCallbackFrames;
		nDroppedFrames = m_nDroppedCallbackFrames;
		bPendingFrameDiscarded = m_pendingFrame.has_value();
		m_pendingFrame.reset();
		++m_nEpoch;
		m_spRenderSession->bActive.store(false);
		m_spRenderSession = std::make_shared<KVideoRenderSession>();
		m_spRenderSession->nGeneration = m_nEpoch;
		m_nReceivedCallbackFrames = 0;
		m_nProcessedCallbackFrames = 0;
		m_nDroppedCallbackFrames = 0;
	}

	if (nReceivedFrames > 0 && KLatencyTraceLogger::isEnabled())
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("remote_callback_frame_coalesce_summary"),
			QStringLiteral("stage=conversion_queue receivedTotal=%1 processedTotal=%2 coalescedTotal=%3 pendingDiscarded=%4")
				.arg(nReceivedFrames)
				.arg(nProcessedFrames)
				.arg(nDroppedFrames)
				.arg(bPendingFrameDiscarded ? 1 : 0));
	}
}

void KWebRtcRemoteFrameProcessor::processFrames()
{
	for (;;)
	{
		std::optional<KPendingFrame> pendingFrame;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_frameCondition.wait(lock, [this]()
				{ return m_bStopping || m_pendingFrame.has_value(); });
			if (m_bStopping)
				return;
			pendingFrame = std::move(m_pendingFrame);
			m_pendingFrame.reset();
			++m_nProcessedCallbackFrames;
		}
		decodeAndEmit(pendingFrame->frame, pendingFrame->nCallbackAtMs,
			pendingFrame->nEpoch);
	}
}

void KWebRtcRemoteFrameProcessor::decodeAndEmit(
	const webrtc::VideoFrame &frame,
	qint64 nCallbackAtMs,
	quint64 nEpoch)
{
	const auto prepareStart = std::chrono::steady_clock::now();
	const quint64 nNextFrameIndex = m_nFrameIndex + 1;
	if (nNextFrameIndex % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("remote_frame_recv"),
			QStringLiteral("frame=%1 timestampMs=%2 lowLatencyRender=%3")
				.arg(nNextFrameIndex)
				.arg(frame.timestamp_us() / 1000)
				.arg(frame.render_parameters().use_low_latency_rendering ? 1 : 0));
	}

	webrtc::scoped_refptr<webrtc::I420BufferInterface> spI420 =
		frame.video_frame_buffer()->ToI420();
	if (spI420 == nullptr)
		return;

	KDecodedVideoFrame decodedFrame;
	decodedFrame.nWidth = spI420->width();
	decodedFrame.nHeight = spI420->height();
	decodedFrame.nFrameIndex = ++m_nFrameIndex;
	decodedFrame.nTimestampMs = frame.timestamp_us() / 1000;
	decodedFrame.bWebRtcLowLatencyRender =
		frame.render_parameters().use_low_latency_rendering;
	decodedFrame.nRemoteCallbackAtMs = nCallbackAtMs;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_bStopping || nEpoch != m_nEpoch)
			return;
		decodedFrame.spRenderSession = m_spRenderSession;
	}
	const bool bLatencyTrace = KLatencyTraceLogger::isEnabled();
	const bool bCpu = g_bForceBgraVideoRenderer.load() || bLatencyTrace
		|| decodedFrame.spRenderSession->bForceBgra.load();
	if (!bCpu)
	{
		decodedFrame.pixelFormat = KDecodedPixelFormat::I420;
		decodedFrame.spPlaneOwner = std::make_shared<webrtc::scoped_refptr<webrtc::I420BufferInterface>>(spI420);
		const int nChromaWidth = decodedFrame.nWidth / 2 + decodedFrame.nWidth % 2;
		const int nChromaHeight = decodedFrame.nHeight / 2 + decodedFrame.nHeight % 2;
		decodedFrame.planes = {{
			{spI420->DataY(), decodedFrame.nWidth, decodedFrame.nHeight, spI420->StrideY(),
				static_cast<size_t>(spI420->StrideY()) * decodedFrame.nHeight},
			{spI420->DataU(), nChromaWidth, nChromaHeight, spI420->StrideU(),
				static_cast<size_t>(spI420->StrideU()) * nChromaHeight},
			{spI420->DataV(), nChromaWidth, nChromaHeight, spI420->StrideV(),
				static_cast<size_t>(spI420->StrideV()) * nChromaHeight}}};
	}
	else
	{
		decodedFrame.spBgraBuffer = std::make_shared<std::vector<unsigned char>>(
			static_cast<size_t>(decodedFrame.nWidth) * decodedFrame.nHeight * 4);

		// libyuv ARGB is stored as B,G,R,A on little-endian Windows, matching DXGI BGRA8.
		const int nConvertResult = libyuv::I420ToARGB(spI420->DataY(),
			spI420->StrideY(),
			spI420->DataU(),
			spI420->StrideU(),
			spI420->DataV(),
			spI420->StrideV(),
			decodedFrame.spBgraBuffer->data(),
			decodedFrame.nWidth * 4,
			decodedFrame.nWidth,
			decodedFrame.nHeight);
		if (nConvertResult != 0)
			return;
		decodedFrame.nConversionDoneAtMs = SteadyNowMs();
	}
	decodedFrame.nFramePreparedAtMs = SteadyNowMs();
	if (m_lastCpuMode != bCpu || m_nLoggedEpoch != nEpoch || decodedFrame.nFrameIndex % 300 == 0)
	{
		if (bLatencyTrace)
		{
			KLatencyTraceLogger::write(QStringLiteral("controller"), QStringLiteral("video_renderer_selected"),
				QStringLiteral("requested=%1 actual=bgra reason=latency_trace generation=%2")
					.arg(g_bForceBgraVideoRenderer.load() ? QStringLiteral("bgra") : QStringLiteral("auto"))
					.arg(nEpoch));
		}
		KSessionTraceLogger::write(QStringLiteral("controller"), QStringLiteral("video_prepare"),
			bCpu ? QStringLiteral("bgra") : QStringLiteral("i420"), -1,
			QStringLiteral("requested=%1 reason=%2 callbackToPreparedMs=%3 processUs=%4 generation=%5")
				.arg(g_bForceBgraVideoRenderer.load() ? QStringLiteral("bgra") : QStringLiteral("auto"))
				.arg(bLatencyTrace ? QStringLiteral("latency_trace") :
					(decodedFrame.spRenderSession->bForceBgra.load() ? QStringLiteral("gpu_fallback") : QStringLiteral("configured")))
				.arg(decodedFrame.nFramePreparedAtMs - nCallbackAtMs)
				.arg(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - prepareStart).count()).arg(nEpoch));
		m_lastCpuMode = bCpu;
		m_nLoggedEpoch = nEpoch;
	}

	if (bLatencyTrace)
	{
		KFrameWatermark watermark;
		if (KFrameWatermarkCodec::readBgra(*decodedFrame.spBgraBuffer,
				decodedFrame.nWidth,
				decodedFrame.nHeight,
				&watermark))
		{
			decodedFrame.nSourceFrameIndex = watermark.nSourceFrameIndex;
			decodedFrame.nLastInputSeq = watermark.nLastInputSeq;
			decodedFrame.nInputAgeMs = watermark.nInputAgeMs;
			KFrameWatermarkCodec::removeBgra(decodedFrame.spBgraBuffer.get(),
				decodedFrame.nWidth,
				decodedFrame.nHeight);
			if (decodedFrame.nFrameIndex % kVideoTraceFrameInterval == 0)
			{
				KLatencyTraceLogger::write(QStringLiteral("controller"),
					QStringLiteral("remote_frame_trace"),
					QStringLiteral("frame=%1 sourceFrame=%2 lastInputSeq=%3 inputAgeMs=%4")
						.arg(decodedFrame.nFrameIndex)
						.arg(decodedFrame.nSourceFrameIndex)
						.arg(decodedFrame.nLastInputSeq)
						.arg(decodedFrame.nInputAgeMs));
			}
		}
	}

	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_bStopping || nEpoch != m_nEpoch)
			return;
	}
	emit frameReady(decodedFrame);
	emit frameStatsReady(decodedFrame.nWidth,
		decodedFrame.nHeight,
		decodedFrame.nFrameIndex,
		decodedFrame.nTimestampMs);
}
