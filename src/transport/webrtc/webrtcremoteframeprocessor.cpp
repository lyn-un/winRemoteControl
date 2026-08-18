#include "transport/webrtc/webrtcremoteframeprocessor.h"

#include "common/framewatermark.h"
#include "common/latencytracelogger.h"

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
	m_processThread = std::thread(
		&KWebRtcRemoteFrameProcessor::processFrames, this);
}

KWebRtcRemoteFrameProcessor::~KWebRtcRemoteFrameProcessor()
{
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		m_bStopping = true;
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

	if (KLatencyTraceLogger::isEnabled())
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
