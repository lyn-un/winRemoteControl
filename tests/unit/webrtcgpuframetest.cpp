#include "transport/webrtc/webrtch264encoder.h"
#include "transport/webrtc/webrtch264decoder.h"
#include "transport/webrtc/webrtcnativeframebuffer.h"
#include "capture/d3d11bgraframe.h"
#include "common/sessiontracelogger.h"

#include <api/make_ref_counted.h>
#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QTemporaryDir>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <iostream>
#include <set>

namespace
{
constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kSubmitted = 96;

class KDecodedCollector final : public webrtc::DecodedImageCallback
{
public:
	int32_t Decoded(webrtc::VideoFrame &frame) override
	{
		const int nIndex = static_cast<int>((frame.rtp_timestamp() - 90000) / 3000);
		auto spI420 = frame.video_frame_buffer()->ToI420();
		if (!spI420 || nIndex < 0 || nIndex >= kSubmitted || frame.width() != kWidth
			|| frame.height() != kHeight || !seen.insert(nIndex).second)
		{
			bValid = false;
			return -1;
		}
		const int nExpected = static_cast<int>(std::lround(16 + 219.0 * (32 + nIndex * 2) / 255));
		nMaxError = (std::max)(nMaxError, std::abs(nExpected - spI420->DataY()[0]));
		return 0;
	}
	std::set<int> seen;
	bool bValid = true;
	int nMaxError = 0;
};

class KEncodedCollector final : public webrtc::EncodedImageCallback
{
public:
	explicit KEncodedCollector(KWebRtcH264Decoder *pDecoder) : m_pDecoder(pDecoder) {}
	Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *) override
	{
		bValid = bValid && m_pDecoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK;
		if (!bValid)
			std::cerr << "decode callback failed rtp=" << image.RtpTimestamp() << std::endl;
		return Result(bValid ? Result::OK : Result::ERROR_SEND_FAILED);
	}
	void OnFrameDropped(uint32_t, int, bool) override {}
	bool bValid = true;
	KWebRtcH264Decoder *m_pDecoder = nullptr;
};

bool TestFallback(ID3D11Texture2D *pTexture, std::shared_ptr<std::recursive_mutex> spMutex, quint64 nLuid)
{
	auto spPath = std::make_shared<KVideoFramePathState>(AutoVideoFramePath);
	spPath->begin(1);
	auto spSource = std::make_shared<KD3D11BgraFrame>(pTexture, spMutex, 1, nLuid);
	auto spBuffer = webrtc::make_ref_counted<KWebRtcNativeFrameBuffer>(spSource, spPath, kWidth, kHeight);
	auto spScaled = spBuffer->Scale(kWidth / 2, kHeight / 2);
	if (!spScaled || spScaled->type() != webrtc::VideoFrameBuffer::Type::kNative
		|| spPath->snapshot().decision != KVideoFramePathState::Undecided)
		return false;
	auto spI420 = spScaled->ToI420();
	if (!spI420 || spI420->width() != kWidth / 2 || spPath->snapshot().decision != KVideoFramePathState::Cpu)
		return false;
	spPath->begin(2);
	// Both a new conversion request and an already cached CPU representation
	// must be rejected after the source session has been replaced.
	if (spBuffer->ToI420() || spScaled->ToI420() || spBuffer->Scale(kWidth / 2, kHeight / 2)
		|| spPath->snapshot().decision != KVideoFramePathState::Undecided)
	{
		return false;
	}
	spPath->selectGpu(2);
	if (spBuffer->ToI420() || spPath->snapshot().decision != KVideoFramePathState::Gpu)
		return false;
	auto spForced = std::make_shared<KVideoFramePathState>(GpuVideoFramePath);
	spForced->begin(1);
	auto spForcedBuffer = webrtc::make_ref_counted<KWebRtcNativeFrameBuffer>(spSource, spForced, kWidth, kHeight);
	if (spForcedBuffer->ToI420() || spForced->snapshot().decision != KVideoFramePathState::Failed)
		return false;
	std::cout << "nativeFallback=auto_i420,forced_gpu_reject,gpu_scale_without_readback,stale_generation_reject"
		<< std::endl;
	return true;
}
}

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	const QString strBackend = application.arguments().value(1);
	const bool bCompatibility = strBackend == QStringLiteral("--compat");
	QTemporaryDir temporary(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gpu-native-XXXXXX")));
	if (!temporary.isValid())
		return 1;
	const QString strLogDirectory = temporary.path();
	KSessionTraceLogger::configure(true, strLogDirectory);
	const auto flushTrace = qScopeGuard([strLogDirectory]()
	{
		KSessionTraceLogger::configure(false, strLogDirectory);
		QFile file(QDir(strLogDirectory).filePath(QStringLiteral("session_trace_controlled.log")));
		if (file.open(QIODevice::ReadOnly))
			std::cout << file.readAll().right(7000).constData() << std::endl;
	});
	const UINT nVendor = strBackend == QStringLiteral("h264_amf") ? 0x1002
		: (strBackend == QStringLiteral("h264_nvenc") ? 0x10de : 0x8086);
	if (!bCompatibility && qEnvironmentVariableIntValue("WRC_RUN_GPU_FRAME_TEST") != 1)
		return 77;
	Microsoft::WRL::ComPtr<ID3D11Device> spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> spContext;
	quint64 nLuid = 0;
	if (bCompatibility)
	{
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			nullptr, 0, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext)))
			return 77;
	}
	else
	{
		Microsoft::WRL::ComPtr<IDXGIFactory1> spFactory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&spFactory))))
			return 1;
		for (UINT nAdapter = 0; ; ++nAdapter)
		{
			Microsoft::WRL::ComPtr<IDXGIAdapter1> spAdapter;
			const HRESULT hr = spFactory->EnumAdapters1(nAdapter, &spAdapter);
			if (hr == DXGI_ERROR_NOT_FOUND)
				return 77;
			if (FAILED(hr))
				return 1;
			DXGI_ADAPTER_DESC1 descriptor = {};
			if (FAILED(spAdapter->GetDesc1(&descriptor)))
				return 1;
			if (descriptor.VendorId != nVendor || (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				continue;
			if (FAILED(D3D11CreateDevice(spAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
				nullptr, 0, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext)))
				return 1;
			nLuid = (static_cast<quint64>(static_cast<quint32>(descriptor.AdapterLuid.HighPart)) << 32)
				| descriptor.AdapterLuid.LowPart;
			std::cout << "gpu=" << QString::fromWCharArray(descriptor.Description).toStdString() << std::endl;
			break;
		}
	}
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = kWidth;
	descriptor.Height = kHeight;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spTexture;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> spView;
	if (FAILED(spDevice->CreateTexture2D(&descriptor, nullptr, &spTexture))
		|| FAILED(spDevice->CreateRenderTargetView(spTexture.Get(), nullptr, &spView)))
		return 1;
	auto spMutex = std::make_shared<std::recursive_mutex>();
	const float black[4] = {0, 0, 0, 1};
	spContext->ClearRenderTargetView(spView.Get(), black);
	if (bCompatibility)
		return TestFallback(spTexture.Get(), spMutex, nLuid) ? 0 : 1;
	auto spPath = std::make_shared<KVideoFramePathState>(GpuVideoFramePath);
	spPath->begin(7);
	spPath->setSeparateCursor(7, true);
	const KVideoEncoderPreference preference = nVendor == 0x1002 ? AmfVideoEncoderPreference
		: (nVendor == 0x10de ? NvencVideoEncoderPreference : QuickSyncVideoEncoderPreference);
	KWebRtcH264Encoder encoder(preference, 52, spPath);
	KWebRtcH264Decoder decoder;
	KDecodedCollector decoded;
	KEncodedCollector encoded(&decoder);
	decoder.RegisterDecodeCompleteCallback(&decoded);
	encoder.RegisterEncodeCompleteCallback(&encoded);
	webrtc::VideoCodec codec = {};
	codec.width = kWidth;
	codec.height = kHeight;
	codec.maxFramerate = 30;
	codec.startBitrate = 2000;
	codec.maxBitrate = 6000;
	const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
	if (!decoder.Configure({}) || encoder.InitEncode(&codec, settings) != WEBRTC_VIDEO_CODEC_OK
		|| !encoder.GetEncoderInfo().supports_native_handle)
	{
		std::cerr << "native codec initialization failed" << std::endl;
		return 1;
	}
	for (int nIndex = 0; nIndex < kSubmitted; ++nIndex)
	{
		const float fGray = (32 + nIndex * 2) / 255.0f;
		const float color[4] = {fGray, fGray, fGray, 1};
		{
			std::lock_guard<std::recursive_mutex> guard(*spMutex);
			spContext->ClearRenderTargetView(spView.Get(), color);
			spContext->Flush();
		}
		auto spSource = std::make_shared<KD3D11BgraFrame>(spTexture.Get(), spMutex, 7, nLuid);
		auto spBuffer = webrtc::make_ref_counted<KWebRtcNativeFrameBuffer>(spSource, spPath, kWidth, kHeight);
		const auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(spBuffer)
			.set_timestamp_ms(1000 + nIndex * 1000LL / 30).set_rtp_timestamp(90000 + nIndex * 3000).build();
		const std::vector<webrtc::VideoFrameType> types{nIndex == 45 ? webrtc::VideoFrameType::kVideoFrameKey
			: webrtc::VideoFrameType::kVideoFrameDelta};
		const int nResult = encoder.Encode(frame, &types);
		if (nResult != WEBRTC_VIDEO_CODEC_OK || spPath->snapshot().decision != KVideoFramePathState::Gpu)
		{
			std::cerr << "native encode failed frame=" << nIndex << " result=" << nResult
				<< " decision=" << spPath->snapshot().decision << " reason=" << spPath->snapshot().strReason.toStdString()
				<< " decoded=" << decoded.seen.size() << std::endl;
			return 1;
		}
	}
	// Live adapter teardown intentionally drops pending output callbacks. Six
	// extra input frames allow validating the first 90 without changing teardown.
	for (int nIndex = 0; nIndex < 90; ++nIndex)
	{
		if (!decoded.seen.contains(nIndex))
		{
			std::cerr << "missing decoded frame=" << nIndex << " decoded=" << decoded.seen.size()
				<< " valid=" << decoded.bValid << " encodedValid=" << encoded.bValid << std::endl;
			return 1;
		}
	}
	std::cout << "nativeWebRtc backend=" << strBackend.toStdString() << " submitted=" << kSubmitted
		<< " decoded=" << decoded.seen.size() << " first90Complete=1 maxLumaError=" << decoded.nMaxError
		<< " actual=gpu noToI420Fallback=1" << std::endl;
	return decoded.bValid && encoded.bValid && decoded.nMaxError <= 6 ? 0 : 1;
}
