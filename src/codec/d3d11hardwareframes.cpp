#include "codec/d3d11hardwareframes.h"

#include <d3d11.h>
#include <d3d10.h>
#include <wrl/client.h>

extern "C"
{
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <array>
#include <algorithm>

namespace
{
constexpr int kFramePoolSize = 8;

struct KContextLock
{
	std::shared_ptr<std::recursive_mutex> spMutex;
};

void LockContext(void *pOpaque)
{
	static_cast<KContextLock *>(pOpaque)->spMutex->lock();
}

void UnlockContext(void *pOpaque)
{
	static_cast<KContextLock *>(pOpaque)->spMutex->unlock();
}

void FreeContextLock(AVHWDeviceContext *pContext)
{
	std::unique_ptr<KContextLock> upLock(static_cast<KContextLock *>(pContext->user_opaque));
}

bool Check(int nResult, const char *pOperation, QString *pError)
{
	if (nResult >= 0)
		return true;
	if (pError)
	{
		char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(nResult, buffer, sizeof(buffer));
		*pError = QStringLiteral("%1: %2 (%3)").arg(QString::fromLatin1(pOperation),
			QString::fromUtf8(buffer)).arg(nResult);
	}
	return false;
}
}

struct KD3D11HardwareFrames::KState
{
	AVBufferRef *pD3dDevice = nullptr;
	AVBufferRef *pEncoderDevice = nullptr;
	AVBufferRef *pD3dFrames = nullptr;
	AVBufferRef *pEncoderFrames = nullptr;
	std::array<AVFrame *, kFramePoolSize> frames{};
	quint64 nGeneration = 0;
	int nWidth = 0;
	int nHeight = 0;
	bool bQsv = false;
	int nPeakInUse = 0;

	~KState()
	{
		for (AVFrame *&pFrame : frames)
			av_frame_free(&pFrame);
		av_buffer_unref(&pEncoderFrames);
		av_buffer_unref(&pD3dFrames);
		av_buffer_unref(&pEncoderDevice);
		av_buffer_unref(&pD3dDevice);
	}
	AVD3D11VADeviceContext *device() const
	{
		return static_cast<AVD3D11VADeviceContext *>(
			reinterpret_cast<AVHWDeviceContext *>(pD3dDevice->data)->hwctx);
	}
};

KD3D11HardwareFrame::KD3D11HardwareFrame(AVFrame *pSource, AVFrame *pEncoder, quint64 nGeneration)
	: m_pSource(pSource)
	, m_pEncoder(pEncoder)
	, m_nGeneration(nGeneration)
{
}

KD3D11HardwareFrame::~KD3D11HardwareFrame()
{
	av_frame_free(&m_pEncoder);
	av_frame_free(&m_pSource);
}

ID3D11Texture2D *KD3D11HardwareFrame::texture() const
{
	return reinterpret_cast<ID3D11Texture2D *>(m_pSource->data[0]);
}

AVFrame *KD3D11HardwareFrame::encoderFrame() const
{
	return m_pEncoder;
}

quint64 KD3D11HardwareFrame::generation() const
{
	return m_nGeneration;
}

KD3D11HardwareFrames::KD3D11HardwareFrames() = default;
KD3D11HardwareFrames::~KD3D11HardwareFrames() = default;

void KD3D11HardwareFrames::reset()
{
	m_upState.reset();
}

bool KD3D11HardwareFrames::initialize(ID3D11Device *pDevice, const QString &strEncoder,
	int nWidth, int nHeight, quint64 nGeneration, QString *pError,
	std::shared_ptr<std::recursive_mutex> spContextMutex)
{
	reset();
	if (!pDevice || nWidth < 2 || nHeight < 2 || nWidth > 16384 || nHeight > 16384
		|| (nWidth & 1) || (nHeight & 1) || nGeneration == 0
		|| (strEncoder != QStringLiteral("h264_qsv") && strEncoder != QStringLiteral("h264_amf")
			&& strEncoder != QStringLiteral("h264_nvenc")))
	{
		if (pError)
			*pError = QStringLiteral("Invalid D3D11 hardware frame configuration");
		return false;
	}
	auto upState = std::make_unique<KState>();
	upState->nWidth = nWidth;
	upState->nHeight = nHeight;
	upState->nGeneration = nGeneration;
	upState->bQsv = strEncoder == QStringLiteral("h264_qsv");
	if (upState->bQsv && !(pDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_VIDEO_SUPPORT))
	{
		if (pError)
			*pError = QStringLiteral("QSV GPU input requires D3D11_CREATE_DEVICE_VIDEO_SUPPORT");
		return false;
	}
	// Mirror FFmpeg's own D3D11 device creation. Driver/VPL internal accesses
	// are outside our shared recursive mutex and also need native protection.
	Microsoft::WRL::ComPtr<ID3D10Multithread> spMultithread;
	if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&spMultithread))))
	{
		if (pError)
			*pError = QStringLiteral("D3D11 multithread protection is unavailable");
		return false;
	}
	spMultithread->SetMultithreadProtected(TRUE);
	upState->pD3dDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if (!upState->pD3dDevice)
		return Check(AVERROR(ENOMEM), "Allocate D3D11 device context", pError);
	// FFmpeg takes ownership of this reference and installs its recursive context
	// lock. All producer operations must use lockContext()/unlockContext().
	upState->device()->device = pDevice;
	pDevice->AddRef();
	if (spContextMutex)
	{
		auto upLock = std::make_unique<KContextLock>();
		upLock->spMutex = std::move(spContextMutex);
		upState->device()->lock = LockContext;
		upState->device()->unlock = UnlockContext;
		upState->device()->lock_ctx = upLock.get();
		auto *pHardware = reinterpret_cast<AVHWDeviceContext *>(upState->pD3dDevice->data);
		pHardware->user_opaque = upLock.release();
		pHardware->free = FreeContextLock;
	}
	if (!Check(av_hwdevice_ctx_init(upState->pD3dDevice), "Initialize shared D3D11 device", pError))
		return false;
	upState->pD3dFrames = av_hwframe_ctx_alloc(upState->pD3dDevice);
	if (!upState->pD3dFrames)
		return Check(AVERROR(ENOMEM), "Allocate D3D11 frame context", pError);
	auto *pFrames = reinterpret_cast<AVHWFramesContext *>(upState->pD3dFrames->data);
	pFrames->format = AV_PIX_FMT_D3D11;
	pFrames->sw_format = AV_PIX_FMT_NV12;
	pFrames->width = (nWidth + 15) & ~15;
	pFrames->height = (nHeight + 15) & ~15;
	pFrames->initial_pool_size = 0;
	static_cast<AVD3D11VAFramesContext *>(pFrames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
	if (!Check(av_hwframe_ctx_init(upState->pD3dFrames), "Initialize NV12 frame context", pError))
		return false;
	if (upState->bQsv)
	{
		if (!Check(av_hwdevice_ctx_create_derived(&upState->pEncoderDevice, AV_HWDEVICE_TYPE_QSV,
			upState->pD3dDevice, 0), "Derive QSV device", pError)
			|| !Check(av_hwframe_ctx_create_derived(&upState->pEncoderFrames, AV_PIX_FMT_QSV,
				upState->pEncoderDevice, upState->pD3dFrames,
				AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT), "Derive QSV frames", pError))
			return false;
	}
	else
	{
		upState->pEncoderDevice = av_buffer_ref(upState->pD3dDevice);
		upState->pEncoderFrames = av_buffer_ref(upState->pD3dFrames);
	}
	if (!upState->pEncoderDevice || !upState->pEncoderFrames)
		return Check(AVERROR(ENOMEM), "Reference hardware contexts", pError);
	for (AVFrame *&pFrame : upState->frames)
	{
		pFrame = av_frame_alloc();
		if (!pFrame)
			return Check(AVERROR(ENOMEM), "Allocate hardware frame", pError);
		if (!Check(av_hwframe_get_buffer(upState->pD3dFrames, pFrame, 0), "Allocate NV12 texture", pError))
			return false;
		pFrame->width = nWidth;
		pFrame->height = nHeight;
	}
	m_upState = std::move(upState);
	return true;
}

KD3D11HardwareFrames::AcquireResult KD3D11HardwareFrames::acquire(
	std::shared_ptr<KD3D11HardwareFrame> *pFrame, QString *pError)
{
	if (pFrame)
		pFrame->reset();
	if (!pFrame || !m_upState)
	{
		if (pError)
			*pError = QStringLiteral("D3D11 frame pool is not initialized");
		return FailedResult;
	}
	int nInUse = 0;
	for (AVFrame *pSlot : m_upState->frames)
	{
		if (!av_buffer_is_writable(pSlot->buf[0]))
			++nInUse;
	}
	for (AVFrame *pSlot : m_upState->frames)
	{
		if (!av_buffer_is_writable(pSlot->buf[0]))
			continue;
		AVFrame *pSource = av_frame_clone(pSlot);
		AVFrame *pEncoder = av_frame_alloc();
		if (!pSource || !pEncoder)
		{
			av_frame_free(&pSource);
			av_frame_free(&pEncoder);
			Check(AVERROR(ENOMEM), "Reference hardware frame", pError);
			return FailedResult;
		}
		int nResult = 0;
		if (m_upState->bQsv)
		{
			pEncoder->hw_frames_ctx = av_buffer_ref(m_upState->pEncoderFrames);
			pEncoder->format = AV_PIX_FMT_QSV;
			nResult = pEncoder->hw_frames_ctx ? av_hwframe_map(pEncoder, pSource,
				AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) : AVERROR(ENOMEM);
		}
		else
			nResult = av_frame_ref(pEncoder, pSource);
		if (!Check(nResult, "Map encoder hardware frame", pError))
		{
			av_frame_free(&pSource);
			av_frame_free(&pEncoder);
			return FailedResult;
		}
		*pFrame = std::shared_ptr<KD3D11HardwareFrame>(
			new KD3D11HardwareFrame(pSource, pEncoder, m_upState->nGeneration));
		m_upState->nPeakInUse = (std::max)(m_upState->nPeakInUse, (std::min)(kFramePoolSize, nInUse + 1));
		return AcquiredResult;
	}
	return PoolBusyResult;
}

int KD3D11HardwareFrames::capacity() const
{
	return m_upState ? kFramePoolSize : 0;
}

int KD3D11HardwareFrames::peakInUse() const
{
	return m_upState ? m_upState->nPeakInUse : 0;
}

AVBufferRef *KD3D11HardwareFrames::deviceContext() const
{
	return m_upState ? m_upState->pEncoderDevice : nullptr;
}

AVBufferRef *KD3D11HardwareFrames::framesContext() const
{
	return m_upState ? m_upState->pEncoderFrames : nullptr;
}

ID3D11DeviceContext *KD3D11HardwareFrames::immediateContext() const
{
	return m_upState ? m_upState->device()->device_context : nullptr;
}

void KD3D11HardwareFrames::lockContext()
{
	m_upState->device()->lock(m_upState->device()->lock_ctx);
}

void KD3D11HardwareFrames::unlockContext()
{
	m_upState->device()->unlock(m_upState->device()->lock_ctx);
}
