#include "transport/webrtc/webrtcnativeframebuffer.h"

#include "capture/d3d11bgraframe.h"
#include "common/sessiontracelogger.h"

#include <api/make_ref_counted.h>
#include <api/video/i420_buffer.h>

extern "C"
{
#include <libswscale/swscale.h>
}

KWebRtcNativeFrameBuffer::KWebRtcNativeFrameBuffer(std::shared_ptr<IKNativeVideoFrame> spFrame,
	std::shared_ptr<KVideoFramePathState> spPath, int nWidth, int nHeight)
	: m_spFrame(std::move(spFrame))
	, m_spPath(std::move(spPath))
	, m_nWidth(nWidth)
	, m_nHeight(nHeight)
{
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> KWebRtcNativeFrameBuffer::ToI420()
{
	std::lock_guard<std::mutex> guard(m_mutex);
	if (!m_spFrame || !m_spPath || m_nWidth < 2 || m_nHeight < 2)
		return nullptr;
	const auto path = m_spPath->snapshot();
	if (path.nGeneration != m_spFrame->generation() || path.decision == KVideoFramePathState::Failed)
		return nullptr;
	if (m_spI420)
		return m_spI420;
	m_spPath->fallback(m_spFrame->generation(), QStringLiteral("native_to_i420_requested"));
	if (m_spPath->snapshot().decision == KVideoFramePathState::Failed)
		return nullptr;
	if (m_spFrame->nativeType() != IKNativeVideoFrame::D3D11BgraNativeType)
		return nullptr;
	const auto *pFrame = static_cast<const KD3D11BgraFrame *>(m_spFrame.get());
	std::vector<unsigned char> pixels;
	if (FAILED(pFrame->copyBgra(&pixels)))
		return nullptr;
	auto spI420 = webrtc::I420Buffer::Create(m_nWidth, m_nHeight);
	SwsContext *pConverter = sws_getContext(pFrame->width(), pFrame->height(), AV_PIX_FMT_BGRA,
		m_nWidth, m_nHeight, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!spI420 || !pConverter)
	{
		sws_freeContext(pConverter);
		return nullptr;
	}
	const unsigned char *source[4] = {pixels.data(), nullptr, nullptr, nullptr};
	const int sourceStride[4] = {pFrame->width() * 4, 0, 0, 0};
	unsigned char *destination[4] = {spI420->MutableDataY(), spI420->MutableDataU(), spI420->MutableDataV(), nullptr};
	const int destinationStride[4] = {spI420->StrideY(), spI420->StrideU(), spI420->StrideV(), 0};
	const int nRows = sws_scale(pConverter, source, sourceStride, 0, pFrame->height(), destination, destinationStride);
	sws_freeContext(pConverter);
	KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("native_frame_cpu_fallback"),
		QStringLiteral("generation=%1 readbackBytes=%2 conversion=bgra_to_i420 reason=%3")
			.arg(pFrame->generation()).arg(pixels.size()).arg(m_spPath->snapshot().strReason));
	if (nRows != m_nHeight)
		return nullptr;
	const auto completedPath = m_spPath->snapshot();
	if (completedPath.nGeneration != pFrame->generation()
		|| completedPath.decision != KVideoFramePathState::Cpu)
	{
		return nullptr;
	}
	m_spI420 = spI420;
	return m_spI420;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> KWebRtcNativeFrameBuffer::CropAndScale(
	int nOffsetX, int nOffsetY, int nCropWidth, int nCropHeight, int nScaledWidth, int nScaledHeight)
{
	if (!m_spFrame || !m_spPath)
		return nullptr;
	const auto path = m_spPath->snapshot();
	if (path.nGeneration != m_spFrame->generation() || path.decision == KVideoFramePathState::Failed)
		return nullptr;
	if (nOffsetX != 0 || nOffsetY != 0 || nCropWidth != m_nWidth || nCropHeight != m_nHeight
		|| nScaledWidth < 2 || nScaledHeight < 2 || (nScaledWidth & 1) || (nScaledHeight & 1))
	{
		if (m_spFrame && m_spPath)
			m_spPath->fallback(m_spFrame->generation(), QStringLiteral("unsupported_native_crop"));
		return nullptr;
	}
	return webrtc::make_ref_counted<KWebRtcNativeFrameBuffer>(m_spFrame, m_spPath, nScaledWidth, nScaledHeight);
}
