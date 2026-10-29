// GPU-only synthetic input probe. No desktop capture or input injection.
#include "capture/gpunv12converter.h"
#include "codec/d3d11hardwareframes.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

#include <QtCore/QCoreApplication>

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFrameCount = 90;
constexpr int kPoolSize = 8;

struct KFrameDeleter
{
	void operator()(AVFrame *pFrame) const { av_frame_free(&pFrame); }
};
struct KCodecDeleter
{
	void operator()(AVCodecContext *pCodec) const { avcodec_free_context(&pCodec); }
};
struct KPacketDeleter
{
	void operator()(AVPacket *pPacket) const { av_packet_free(&pPacket); }
};
struct KBuffer
{
	AVBufferRef *pReference = nullptr;
	~KBuffer() { av_buffer_unref(&pReference); }
	KBuffer() = default;
	KBuffer(const KBuffer &) = delete;
	KBuffer &operator=(const KBuffer &) = delete;
};
using KFrame = std::unique_ptr<AVFrame, KFrameDeleter>;
using KCodec = std::unique_ptr<AVCodecContext, KCodecDeleter>;
using KPacket = std::unique_ptr<AVPacket, KPacketDeleter>;

bool Check(int nResult, const char *pOperation)
{
	if (nResult >= 0)
		return true;
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(nResult, buffer, sizeof(buffer));
	std::cerr << pOperation << " failed: " << buffer << " code=" << nResult << std::endl;
	return false;
}

int ExpectedY(int nIndex)
{
	return 32 + (nIndex / 30) * 80 + nIndex % 30;
}

struct KProbe
{
	KBuffer d3dDevice;
	KD3D11HardwareFrames pool;
	KCodec encoder;
	KCodec decoder;
	KPacket packet{av_packet_alloc()};
	KFrame decoded{av_frame_alloc()};
	std::array<bool, kFrameCount> seen{};
	int nDecoded = 0;
	int nMaxError = 0;
	bool bRgbInput = false;
	KGpuNv12Converter converter;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spRgbInput;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> spRgbView;

	bool verifyPool(ID3D11Device *pDevice, const QString &strBackend, QString *pError)
	{
		std::array<std::shared_ptr<KD3D11HardwareFrame>, kPoolSize> held;
		for (auto &spFrame : held)
		{
			if (pool.acquire(&spFrame, pError) != KD3D11HardwareFrames::AcquiredResult)
				return false;
		}
		std::shared_ptr<KD3D11HardwareFrame> spExtra;
		if (pool.capacity() != kPoolSize || pool.peakInUse() != kPoolSize
			|| pool.acquire(&spExtra, pError) != KD3D11HardwareFrames::PoolBusyResult)
			return false;
		// Simulate a downstream encoder retaining an AVFrame after the producer
		// drops its shared_ptr. The slot must remain unavailable.
		KFrame retained{av_frame_clone(held[0]->encoderFrame())};
		held[0].reset();
		if (!retained || pool.acquire(&spExtra, pError) != KD3D11HardwareFrames::PoolBusyResult)
			return false;
		retained.reset();
		if (pool.acquire(&held[0], pError) != KD3D11HardwareFrames::AcquiredResult)
			return false;
		pool.reset();
		for (const auto &spFrame : held)
		{
			D3D11_TEXTURE2D_DESC descriptor = {};
			spFrame->texture()->GetDesc(&descriptor);
			if (descriptor.Format != DXGI_FORMAT_NV12 || spFrame->generation() != 1)
				return false;
		}
		if (!pool.initialize(pDevice, strBackend, kWidth, kHeight, 2, pError)
			|| pool.acquire(&spExtra, pError) != KD3D11HardwareFrames::AcquiredResult
			|| spExtra->generation() != 2 || pool.peakInUse() != 1)
			return false;
		std::cout << "poolChecks=bounded,encoder_reference_retained,late_release,generation_reset" << std::endl;
		return true;
	}

	bool initialize(const QByteArray &strBackend, int nAdapter)
	{
		if (!Check(av_hwdevice_ctx_create(&d3dDevice.pReference, AV_HWDEVICE_TYPE_D3D11VA,
			QByteArray::number(nAdapter).constData(), nullptr, 0), "create D3D11 device"))
			return false;
		const bool bQsv = strBackend == "h264_qsv";
		auto *pDevice = static_cast<AVD3D11VADeviceContext *>(
			reinterpret_cast<AVHWDeviceContext *>(d3dDevice.pReference->data)->hwctx);
		QString strError;
		if (!pool.initialize(pDevice->device, QString::fromLatin1(strBackend), kWidth, kHeight, 1, &strError)
			|| !verifyPool(pDevice->device, QString::fromLatin1(strBackend), &strError))
		{
			std::cerr << strError.toStdString() << std::endl;
			return false;
		}
		if (bRgbInput)
		{
			D3D11_TEXTURE2D_DESC descriptor = {};
			descriptor.Width = kWidth;
			descriptor.Height = kHeight;
			descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
			descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			descriptor.Usage = D3D11_USAGE_DEFAULT;
			descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
			if (FAILED(pDevice->device->CreateTexture2D(&descriptor, nullptr, &spRgbInput))
				|| FAILED(pDevice->device->CreateRenderTargetView(spRgbInput.Get(), nullptr, &spRgbView)))
				return false;
		}
		encoder.reset(avcodec_alloc_context3(avcodec_find_encoder_by_name(strBackend.constData())));
		decoder.reset(avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264)));
		if (!encoder || !decoder || !packet || !decoded)
			return false;
		encoder->width = kWidth;
		encoder->height = kHeight;
		encoder->pix_fmt = bQsv ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11;
		encoder->hw_device_ctx = av_buffer_ref(pool.deviceContext());
		encoder->hw_frames_ctx = av_buffer_ref(pool.framesContext());
		encoder->time_base = AVRational{1, 30};
		encoder->framerate = AVRational{30, 1};
		encoder->bit_rate = 4'000'000;
		encoder->rc_max_rate = 4'000'000;
		encoder->rc_buffer_size = 1'000'000;
		encoder->gop_size = 60;
		encoder->max_b_frames = 0;
		encoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
		if (bQsv)
		{
			if (!Check(av_opt_set(encoder->priv_data, "preset", "veryfast", 0), "QSV preset")
				|| !Check(av_opt_set_int(encoder->priv_data, "async_depth", 1, 0), "QSV async depth"))
				return false;
		}
		else if (strBackend == "h264_amf")
		{
			if (!Check(av_opt_set(encoder->priv_data, "usage", "ultralowlatency", 0), "AMF usage")
				|| !Check(av_opt_set_int(encoder->priv_data, "async_depth", 1, 0), "AMF async depth"))
				return false;
		}
		else
		{
			if (!Check(av_opt_set(encoder->priv_data, "tune", "ull", 0), "NVENC tune")
				|| !Check(av_opt_set_int(encoder->priv_data, "delay", 0, 0), "NVENC delay"))
				return false;
		}
		decoder->thread_count = 1;
		return Check(avcodec_open2(encoder.get(), encoder->codec, nullptr), "open hardware encoder")
			&& Check(avcodec_open2(decoder.get(), decoder->codec, nullptr), "open software decoder");
	}

	bool receiveDecoded()
	{
		for (;;)
		{
			const int nResult = avcodec_receive_frame(decoder.get(), decoded.get());
			if (nResult == AVERROR(EAGAIN) || nResult == AVERROR_EOF)
				return true;
			if (!Check(nResult, "decode frame"))
				return false;
			const std::int64_t nPts = decoded->pts;
			if (nPts < 0 || nPts >= kFrameCount || seen[nPts] || decoded->width != kWidth
				|| decoded->height != kHeight
				|| (decoded->format != AV_PIX_FMT_YUV420P && decoded->format != AV_PIX_FMT_NV12))
			{
				std::cerr << "Invalid/duplicate decoded frame pts=" << nPts << std::endl;
				return false;
			}
			seen[nPts] = true;
			++nDecoded;
			for (int y = 16; y < kHeight; y += 47)
			{
				for (int x = 16; x < kWidth; x += 61)
				{
					const int nError = std::abs(static_cast<int>(decoded->data[0][y * decoded->linesize[0] + x])
						- (bRgbInput ? static_cast<int>(std::lround(16 + 219.0 * ExpectedY(static_cast<int>(nPts)) / 255))
							: ExpectedY(static_cast<int>(nPts))));
					nMaxError = (std::max)(nMaxError, nError);
				}
			}
			const int nU = decoded->data[1][0];
			const int nV = decoded->format == AV_PIX_FMT_NV12 ? decoded->data[1][1] : decoded->data[2][0];
			nMaxError = (std::max)(nMaxError, (std::max)(std::abs(nU - (bRgbInput ? 128 : 90)),
				std::abs(nV - (bRgbInput ? 128 : 170))));
			av_frame_unref(decoded.get());
		}
	}

	bool receivePackets()
	{
		for (;;)
		{
			const int nResult = avcodec_receive_packet(encoder.get(), packet.get());
			if (nResult == AVERROR(EAGAIN) || nResult == AVERROR_EOF)
				return true;
			if (!Check(nResult, "receive encoded packet")
				|| !Check(avcodec_send_packet(decoder.get(), packet.get()), "submit decode packet"))
				return false;
			av_packet_unref(packet.get());
			if (!receiveDecoded())
				return false;
		}
	}

	bool run()
	{
		auto *pDevice = static_cast<AVD3D11VADeviceContext *>(
			reinterpret_cast<AVHWDeviceContext *>(d3dDevice.pReference->data)->hwctx);
		for (int nIndex = 0; nIndex < kFrameCount; ++nIndex)
		{
			QString strError;
			std::shared_ptr<KD3D11HardwareFrame> spInput;
			if (pool.acquire(&spInput, &strError) != KD3D11HardwareFrames::AcquiredResult)
			{
				std::cerr << "Acquire bounded GPU frame failed: " << strError.toStdString() << std::endl;
				return false;
			}
			const float yColor[4] = {ExpectedY(nIndex) / 255.0f, 0, 0, 1};
			const float uvColor[4] = {90.0f / 255, 170.0f / 255, 0, 1};
			pool.lockContext();
			ID3D11DeviceContext *pContext = pool.immediateContext();
			HRESULT hr = S_OK;
			if (bRgbInput)
			{
				const float color[4] = {yColor[0], yColor[0], yColor[0], 1};
				pContext->ClearRenderTargetView(spRgbView.Get(), color);
				hr = converter.convert(pContext, spRgbInput.Get(), spInput->texture(), kWidth, kHeight);
			}
			else
			{
				Microsoft::WRL::ComPtr<ID3D11RenderTargetView> spY, spUv;
				D3D11_RENDER_TARGET_VIEW_DESC descriptor = {};
				descriptor.Format = DXGI_FORMAT_R8_UNORM;
				descriptor.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				hr = pDevice->device->CreateRenderTargetView(spInput->texture(), &descriptor, &spY);
				if (SUCCEEDED(hr))
				{
					descriptor.Format = DXGI_FORMAT_R8G8_UNORM;
					hr = pDevice->device->CreateRenderTargetView(spInput->texture(), &descriptor, &spUv);
				}
				if (SUCCEEDED(hr))
				{
					pContext->ClearRenderTargetView(spY.Get(), yColor);
					pContext->ClearRenderTargetView(spUv.Get(), uvColor);
				}
			}
			pContext->Flush();
			pool.unlockContext();
			if (FAILED(hr))
			{
				std::cerr << "GPU RGB to NV12 failed HRESULT=" << std::hex << hr << std::dec << std::endl;
				return false;
			}
			AVFrame *pInput = spInput->encoderFrame();
			pInput->pts = nIndex;
			int nResult = avcodec_send_frame(encoder.get(), pInput);
			if (nResult == AVERROR(EAGAIN))
			{
				if (!receivePackets())
					return false;
				nResult = avcodec_send_frame(encoder.get(), pInput);
			}
			if (!Check(nResult, "submit GPU frame") || !receivePackets())
				return false;
		}
		if (!Check(avcodec_send_frame(encoder.get(), nullptr), "drain encoder") || !receivePackets()
			|| !Check(avcodec_send_packet(decoder.get(), nullptr), "drain decoder") || !receiveDecoded())
			return false;
		std::cout << "decoded=" << nDecoded << " expected=" << kFrameCount
			<< " maxSampleError=" << nMaxError
			<< " allocatedTextures=" << kPoolSize
			<< " syntheticInput=" << (bRgbInput ? "GPU_RGB_to_NV12" : "GPU_NV12_clear")
			<< " inputReadbackBytes=0 cpuPixelUploadBytes=0" << std::endl;
		return nDecoded == kFrameCount && nMaxError <= 6;
	}
};
}

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	if (qEnvironmentVariableIntValue("WRC_RUN_GPU_FRAME_TEST") != 1)
		return 77;
	const QByteArray strBackend = application.arguments().value(1).toLatin1();
	const UINT nVendor = strBackend == "h264_amf" ? 0x1002
		: (strBackend == "h264_nvenc" ? 0x10de : (strBackend == "h264_qsv" ? 0x8086 : 0));
	if (!nVendor)
		return 2;
	Microsoft::WRL::ComPtr<IDXGIFactory1> spFactory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&spFactory))))
		return 1;
	for (UINT nIndex = 0; ; ++nIndex)
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter1> spAdapter;
		const HRESULT hr = spFactory->EnumAdapters1(nIndex, &spAdapter);
		if (hr == DXGI_ERROR_NOT_FOUND)
			return 77;
		if (FAILED(hr))
			return 1;
		DXGI_ADAPTER_DESC1 descriptor = {};
		if (FAILED(spAdapter->GetDesc1(&descriptor)))
			return 1;
		if (descriptor.VendorId != nVendor || (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			continue;
		std::cout << "backend=" << strBackend.constData() << " adapter=" << nIndex
			<< " gpu=" << QString::fromWCharArray(descriptor.Description).toStdString()
			<< " luidHigh=" << descriptor.AdapterLuid.HighPart
			<< " luidLow=" << descriptor.AdapterLuid.LowPart
			<< " ffmpeg=" << av_version_info() << std::endl;
		KProbe probe;
		probe.bRgbInput = application.arguments().contains(QStringLiteral("--rgb"));
		return probe.initialize(strBackend, static_cast<int>(nIndex)) && probe.run() ? 0 : 1;
	}
}
