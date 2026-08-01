#include "transport/webrtc/webrtcremoteframeprocessor.h"

#include "common/framewatermark.h"
#include "common/latencytracelogger.h"

#include <QtCore/QMetaObject>

#include <api/video/i420_buffer.h>

#include <libyuv.h>

#include <utility>

namespace
{
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr quint64 kCallbackFrameCoalesceTraceInterval = 30;
}

KWebRtcRemoteFrameProcessor::KWebRtcRemoteFrameProcessor(QObject *pParent)
	: QObject(pParent)
{
}

void KWebRtcRemoteFrameProcessor::enqueue(const webrtc::VideoFrame &frame)
{
	bool bNeedQueue = false;
	bool bFrameCoalesced = false;
	quint64 nReceivedFrames = 0;
	quint64 nProcessedFrames = 0;
	quint64 nDroppedFrames = 0;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		++m_nReceivedCallbackFrames;
		if (m_bHasPendingFrame)
		{
			++m_nDroppedCallbackFrames;
			bFrameCoalesced = true;
		}
		m_pendingFrame = frame;
		m_bHasPendingFrame = true;
		nReceivedFrames = m_nReceivedCallbackFrames;
		nProcessedFrames = m_nProcessedCallbackFrames;
		nDroppedFrames = m_nDroppedCallbackFrames;
		if (!m_bProcessQueued)
		{
			m_bProcessQueued = true;
			bNeedQueue = true;
		}
	}

	if (bFrameCoalesced
		&& KLatencyTraceLogger::isEnabled()
		&& (nDroppedFrames == 1
			|| nDroppedFrames % kCallbackFrameCoalesceTraceInterval == 0))
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("remote_callback_frame_coalesced"),
			QStringLiteral("receivedTotal=%1 processedTotal=%2 coalescedTotal=%3 latestTimestampMs=%4")
				.arg(nReceivedFrames)
				.arg(nProcessedFrames)
				.arg(nDroppedFrames)
				.arg(frame.timestamp_us() / 1000));
	}

	if (bNeedQueue)
	{
		QMetaObject::invokeMethod(this,
			[this]() { processLatest(); },
			Qt::QueuedConnection);
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
		bPendingFrameDiscarded = m_bHasPendingFrame;
		m_pendingFrame.reset();
		m_bHasPendingFrame = false;
		m_bProcessQueued = false;
		m_nReceivedCallbackFrames = 0;
		m_nProcessedCallbackFrames = 0;
		m_nDroppedCallbackFrames = 0;
	}

	if (nReceivedFrames > 0 && KLatencyTraceLogger::isEnabled())
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("remote_callback_frame_coalesce_summary"),
			QStringLiteral("receivedTotal=%1 processedTotal=%2 coalescedTotal=%3 pendingDiscarded=%4")
				.arg(nReceivedFrames)
				.arg(nProcessedFrames)
				.arg(nDroppedFrames)
				.arg(bPendingFrameDiscarded ? 1 : 0));
	}
}

void KWebRtcRemoteFrameProcessor::processLatest()
{
	std::optional<webrtc::VideoFrame> pendingFrame;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (!m_bHasPendingFrame)
		{
			m_bProcessQueued = false;
			return;
		}
		pendingFrame = std::move(m_pendingFrame);
		m_pendingFrame.reset();
		m_bHasPendingFrame = false;
		++m_nProcessedCallbackFrames;
	}

	if (pendingFrame.has_value())
		decodeAndEmit(*pendingFrame);

	bool bNeedQueue = false;
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		if (m_bHasPendingFrame)
			bNeedQueue = true;
		else
			m_bProcessQueued = false;
	}
	if (bNeedQueue)
	{
		QMetaObject::invokeMethod(this,
			[this]() { processLatest(); },
			Qt::QueuedConnection);
	}
}

void KWebRtcRemoteFrameProcessor::decodeAndEmit(const webrtc::VideoFrame &frame)
{
	const quint64 nNextFrameIndex = m_nFrameIndex + 1;
	if (nNextFrameIndex % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("remote_frame_recv"),
			QStringLiteral("frame=%1 timestampMs=%2")
				.arg(nNextFrameIndex)
				.arg(frame.timestamp_us() / 1000));
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
	decodedFrame.vecBgraBuffer.resize(
		static_cast<size_t>(decodedFrame.nWidth) * decodedFrame.nHeight * 4);

	// libyuv ARGB is stored as B,G,R,A on little-endian Windows, matching DXGI BGRA8.
	const int nConvertResult = libyuv::I420ToARGB(spI420->DataY(),
		spI420->StrideY(),
		spI420->DataU(),
		spI420->StrideU(),
		spI420->DataV(),
		spI420->StrideV(),
		decodedFrame.vecBgraBuffer.data(),
		decodedFrame.nWidth * 4,
		decodedFrame.nWidth,
		decodedFrame.nHeight);
	if (nConvertResult != 0)
		return;

	if (KLatencyTraceLogger::isEnabled())
	{
		KFrameWatermark watermark;
		if (KFrameWatermarkCodec::readBgra(decodedFrame.vecBgraBuffer,
				decodedFrame.nWidth,
				decodedFrame.nHeight,
				&watermark))
		{
			decodedFrame.nSourceFrameIndex = watermark.nSourceFrameIndex;
			decodedFrame.nLastInputSeq = watermark.nLastInputSeq;
			decodedFrame.nInputAgeMs = watermark.nInputAgeMs;
			KFrameWatermarkCodec::removeBgra(&decodedFrame.vecBgraBuffer,
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

	emit frameReady(decodedFrame);
	emit frameStatsReady(decodedFrame.nWidth,
		decodedFrame.nHeight,
		decodedFrame.nFrameIndex,
		decodedFrame.nTimestampMs);
}
