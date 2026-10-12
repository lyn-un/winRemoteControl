#include "capture/hdrgpubgraconverter.h"
#include "capture/mappedframeconversion.h"
#include "capture/dxgidesktopduplicator.h"

#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

#if defined(WRC_HDR_FAULT_TESTS)
namespace
{
const char *g_pFailureStage = nullptr;
int g_nFailureHits = 0;
}

HRESULT HdrCaptureTestFailure(const char *pStage)
{
	if (!g_pFailureStage || std::strcmp(g_pFailureStage, pStage) != 0)
		return S_OK;
	++g_nFailureHits;
	return std::strcmp(pStage, "device") == 0 ? DXGI_ERROR_DEVICE_REMOVED : E_OUTOFMEMORY;
}
#endif

// Exercise the production readback/fallback path with synthetic textures only.
// Never initialize desktop duplication or acquire a real desktop frame here.
struct KHdrCaptureTestAccess
{
	static bool CheckPath(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
		KHdrConversionMode mode, bool bExpectFallback, bool bExpectDeviceLoss = false)
	{
		KDxgiDesktopDuplicator capture(mode);
		capture.m_spDevice = pDevice;
		capture.m_spContext = pContext;
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = 3;
		desc.Height = 2;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		const std::uint16_t pixels[24] = {
			0x3c00, 0x3800, 0, 0, 0x4100, 0x4100, 0x4100, 0, 0, 0, 0, 0,
			0x3c00, 0x3800, 0, 0, 0x4100, 0x4100, 0x4100, 0, 0, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA initial = { pixels, 24, 0 };
		ComPtr<ID3D11Texture2D> spInput;
		if (FAILED(pDevice->CreateTexture2D(&desc, &initial, &spInput)))
			return false;
		KCaptureFrame frame;
		QString error;
		if (bExpectDeviceLoss)
		{
			return !capture.copyTextureToBgraFrame(spInput.Get(), &frame, &error)
				&& !capture.m_bHdrGpuFailed && !capture.m_spStagingTexture
				&& frame.vecBgraBuffer.empty() && !error.isEmpty();
		}
		for (int nFrame = 0; nFrame < 3; ++nFrame)
		{
			if (!capture.copyTextureToBgraFrame(spInput.Get(), &frame, &error)
				|| capture.m_bHdrGpuFailed != bExpectFallback
				|| frame.nWidth != 3 || frame.nHeight != 2
				|| frame.vecBgraBuffer[0] != 0 || frame.vecBgraBuffer[4] != 255)
				return false;
			const bool bCpu = mode == CpuHdrConversionMode || bExpectFallback;
			if (capture.m_stagingDesc.Format != (bCpu
				? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM))
				return false;
		}
		const auto cleanPixels = frame.vecBgraBuffer;
		capture.m_spLastDesktopTexture = spInput;
		capture.m_pointerPosition.Visible = TRUE;
		capture.m_pointerShapeInfo.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
		capture.m_pointerShapeInfo.Width = 1;
		capture.m_pointerShapeInfo.Height = 1;
		capture.m_pointerShapeInfo.Pitch = 4;
		capture.m_vecPointerShapeBuffer = { 10, 20, 30, 255 };
		if (capture.capturePointerOnlyFrame(&frame, &error) != IKCaptureSource::CapturedCaptureResult
			|| frame.vecBgraBuffer[0] != 10 || frame.vecBgraBuffer[2] != 30)
			return false;
		capture.m_pointerPosition.Position.x = 2;
		if (capture.capturePointerOnlyFrame(&frame, &error) != IKCaptureSource::CapturedCaptureResult
			|| frame.vecBgraBuffer[0] != cleanPixels[0] || frame.vecBgraBuffer[8] != 10)
			return false;
		capture.m_pointerPosition.Position.x = 0;
		capture.m_pointerShapeInfo.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
		capture.m_pointerShapeInfo.Height = 2;
		capture.m_pointerShapeInfo.Pitch = 1;
		capture.m_vecPointerShapeBuffer = { 0x80, 0x80 };
		for (int nRepeat = 0; nRepeat < 2; ++nRepeat)
		{
			if (capture.capturePointerOnlyFrame(&frame, &error) != IKCaptureSource::CapturedCaptureResult
				|| frame.vecBgraBuffer[0] != static_cast<unsigned char>(cleanPixels[0] ^ 0xff)
				|| frame.vecBgraBuffer[1] != static_cast<unsigned char>(cleanPixels[1] ^ 0xff))
				return false;
		}
		capture.m_pointerPosition.Visible = FALSE;
		// SDR must bypass HDR conversion without clearing the failure latch.
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		const unsigned char sdr[24] = {};
		initial = { sdr, 12, 0 };
		spInput.Reset();
		if (FAILED(pDevice->CreateTexture2D(&desc, &initial, &spInput))
			|| !capture.copyTextureToBgraFrame(spInput.Get(), &frame, &error)
			|| capture.m_bHdrGpuFailed != bExpectFallback
			|| capture.m_stagingDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
			return false;
		capture.shutdown();
		return !capture.m_bHdrGpuFailed && !capture.m_spStagingTexture
			&& capture.m_lastLoggedFormat == DXGI_FORMAT_UNKNOWN;
	}
};

namespace
{
bool CheckResourceCycles(ID3D11Device *pDevice, ID3D11DeviceContext *pContext)
{
	ComPtr<IDXGIDevice> spDxgiDevice;
	ComPtr<IDXGIAdapter> spAdapter;
	ComPtr<IDXGIAdapter3> spAdapter3;
	if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&spDxgiDevice)))
		&& SUCCEEDED(spDxgiDevice->GetAdapter(&spAdapter)))
		spAdapter.As(&spAdapter3);
	std::vector<std::uint16_t> pixels(2560 * 1440 * 4, 0x3c00);
	KHdrGpuBgraConverter converter;
	SIZE_T nPrivateBaseline = 0;
	UINT64 nVideoBaseline = 0;
	bool bVideoBaseline = false;
	for (int nCycle = 1; nCycle <= 120; ++nCycle)
	{
		{
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = nCycle % 2 ? 1920 : 2560;
			desc.Height = nCycle % 2 ? 1080 : 1440;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			D3D11_SUBRESOURCE_DATA initial = { pixels.data(), desc.Width * 8, 0 };
			ComPtr<ID3D11Texture2D> spInput, spOutput, spStaging;
			if (FAILED(pDevice->CreateTexture2D(&desc, &initial, &spInput))
				|| FAILED(converter.convert(pDevice, pContext, spInput.Get(), 2.5f, &spOutput)))
				return false;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &spStaging)))
				return false;
			pContext->CopyResource(spStaging.Get(), spOutput.Get());
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (FAILED(pContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return false;
			pContext->Unmap(spStaging.Get(), 0);
		}
		converter.reset();
		pContext->Flush();
		if (nCycle % 10 != 0)
			continue;
		PROCESS_MEMORY_COUNTERS_EX memory = {};
		if (!GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
			return false;
		DXGI_QUERY_VIDEO_MEMORY_INFO video = {};
		const bool bVideo = spAdapter3 && SUCCEEDED(spAdapter3->QueryVideoMemoryInfo(
			0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video));
		std::cout << "resourceCycle=" << nCycle << " privateBytes=" << memory.PrivateUsage
			<< " localVideoBytes=" << video.CurrentUsage << " videoAvailable=" << bVideo << '\n';
		if (nCycle == 20)
		{
			nPrivateBaseline = memory.PrivateUsage;
			nVideoBaseline = video.CurrentUsage;
			bVideoBaseline = bVideo;
		}
		// Allow allocator/driver caches, but reject accumulating full-frame allocations.
		if (nCycle > 20 && (memory.PrivateUsage > nPrivateBaseline + 32 * 1024 * 1024
			|| (bVideo && bVideoBaseline && video.CurrentUsage > nVideoBaseline + 64 * 1024 * 1024)))
			return false;
	}
	return true;
}

unsigned char ReferenceChannel(std::uint16_t nBits, float fWhiteScale)
{
	const int nExponent = (nBits >> 10) & 31;
	const int nMantissa = nBits & 1023;
	if (nExponent == 31)
		return nMantissa || (nBits & 0x8000) ? 0 : 255;
	if (nBits & 0x8000)
		return 0;
	float fValue = nExponent ? std::ldexp(static_cast<float>(1024 + nMantissa), nExponent - 25)
		: std::ldexp(static_cast<float>(nMantissa), -24);
	fValue = std::clamp(fValue / fWhiteScale, 0.0f, 1.0f);
	fValue = fValue <= 0.0031308f ? 12.92f * fValue
		: 1.055f * std::pow(fValue, 1.0f / 2.4f) - 0.055f;
	return static_cast<unsigned char>(std::clamp(fValue, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool CheckImage(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
	KHdrGpuBgraConverter &converter, UINT nWidth, UINT nHeight, float fWhiteScale, int &nMaxError,
	bool bRandom = false)
{
	// Include source row padding and different channel sequences to detect swaps.
	const UINT nStride = nWidth * 4 + 12;
	std::vector<std::uint16_t> pixels(static_cast<size_t>(nStride) * nHeight, 0xffff);
	std::uint32_t nRandom = 12345;
	for (UINT y = 0; y < nHeight; ++y)
	{
		for (UINT x = 0; x < nWidth; ++x)
		{
			nRandom = nRandom * 1664525u + 1013904223u;
			const UINT nIndex = bRandom ? nRandom >> 8 : y * nWidth + x;
			const size_t nOffset = static_cast<size_t>(y) * nStride + x * 4;
			pixels[nOffset] = static_cast<std::uint16_t>(nIndex);
			pixels[nOffset + 1] = static_cast<std::uint16_t>(nIndex * 251 + 123);
			pixels[nOffset + 2] = static_cast<std::uint16_t>(65535 - nIndex);
			pixels[nOffset + 3] = 0;
		}
	}
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = nWidth;
	desc.Height = nHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA initial = {};
	initial.pSysMem = pixels.data();
	initial.SysMemPitch = nStride * sizeof(std::uint16_t);
	D3D11_MAPPED_SUBRESOURCE cpuMapped = {};
	cpuMapped.pData = pixels.data();
	cpuMapped.RowPitch = initial.SysMemPitch;
	KCaptureFrame cpuReference;
	if (!CopyMappedCaptureFrameToBgra(desc, cpuMapped, &cpuReference, fWhiteScale))
		return false;
	ComPtr<ID3D11Texture2D> spInput, spOutput, spRepeated, spStaging;
	HRESULT hr = pDevice->CreateTexture2D(&desc, &initial, &spInput);
	if (SUCCEEDED(hr))
		hr = converter.convert(pDevice, pContext, spInput.Get(), fWhiteScale, &spOutput);
	if (FAILED(hr))
	{
		std::cerr << "create/convert failed " << std::hex << hr << '\n';
		return false;
	}
	hr = converter.convert(pDevice, pContext, spInput.Get(), fWhiteScale, &spRepeated);
	if (FAILED(hr) || spRepeated.Get() != spOutput.Get())
		return false;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &spStaging)))
		return false;
	pContext->CopyResource(spStaging.Get(), spOutput.Get());
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(pContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
		return false;
	bool bValid = true;
	for (UINT y = 0; y < nHeight; ++y)
	{
		const auto *pRow = static_cast<const unsigned char *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
		for (UINT x = 0; x < nWidth; ++x)
		{
			for (UINT nChannel = 0; nChannel < 3; ++nChannel)
			{
				const auto nBits = pixels[static_cast<size_t>(y) * nStride + x * 4 + 2 - nChannel];
				const int nError = std::abs(static_cast<int>(pRow[x * 4 + nChannel])
					- ReferenceChannel(nBits, fWhiteScale));
				if (cpuReference.vecBgraBuffer[(static_cast<size_t>(y) * nWidth + x) * 4 + nChannel]
					!= ReferenceChannel(nBits, fWhiteScale))
					bValid = false;
				nMaxError = std::max(nMaxError, nError);
				bValid = bValid && nError <= 1;
			}
			bValid = bValid && pRow[x * 4 + 3] == 255;
		}
	}
	pContext->Unmap(spStaging.Get(), 0);
	ComPtr<ID3D11Texture2D> spInvalid;
	if (converter.convert(pDevice, pContext, spInput.Get(), 0, &spInvalid) != E_INVALIDARG || spInvalid)
		bValid = false;
	if (converter.convert(pDevice, pContext, spStaging.Get(), fWhiteScale, &spInvalid) != E_INVALIDARG)
		bValid = false;
	if (converter.convert(pDevice, pContext, spInput.Get(), fWhiteScale, nullptr) != E_POINTER)
		bValid = false;
	for (float fInvalidScale : { -1.0f, std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN() })
	{
		if (converter.convert(pDevice, pContext, spInput.Get(), fInvalidScale, &spInvalid) != E_INVALIDARG
			|| spInvalid)
			bValid = false;
	}
	// Rejected arguments must not corrupt the reusable resources.
	spRepeated.Reset();
	if (FAILED(converter.convert(pDevice, pContext, spInput.Get(), fWhiteScale, &spRepeated))
		|| spRepeated.Get() != spOutput.Get())
		bValid = false;
	return bValid;
}
}

int main(int nArgc, char **pArgv)
{
	const bool bWarp = nArgc > 1 && std::string(pArgv[1]) == "--warp";
	const bool bResources = nArgc > 1 && std::string(pArgv[1]) == "--resources";
	ComPtr<ID3D11Device> spDevice;
	ComPtr<ID3D11DeviceContext> spContext;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
	UINT nFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
	HRESULT hr = D3D11CreateDevice(nullptr, bWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, nFlags, levels, 1, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext);
	if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
	{
		nFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
		hr = D3D11CreateDevice(nullptr, bWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
			nullptr, nFlags, levels, 1, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext);
	}
	if (FAILED(hr))
	{
		std::cout << "SKIP device unavailable: " << std::hex << hr << '\n';
		return 77;
	}
	if (bResources)
		return CheckResourceCycles(spDevice.Get(), spContext.Get()) ? 0 : 1;
	ComPtr<ID3D11InfoQueue> spMessages;
	spDevice.As(&spMessages);
	int nMaxError = 0;
	KHdrGpuBgraConverter converter;
	if (!CheckImage(spDevice.Get(), spContext.Get(), converter, 256, 256, 2.5f, nMaxError)
		|| !CheckImage(spDevice.Get(), spContext.Get(), converter, 257, 19, 2.5f, nMaxError)
		|| !CheckImage(spDevice.Get(), spContext.Get(), converter, 127, 61, 2.5f, nMaxError, true)
		|| !CheckImage(spDevice.Get(), spContext.Get(), converter, 1, 257, 1.0f, nMaxError))
	{
		std::cerr << "Image comparison failed, max error=" << nMaxError << '\n';
		return 1;
	}
	for (int nCycle = 0; nCycle < 20; ++nCycle)
	{
		converter.reset();
		spContext->Flush();
		spDevice->AddRef();
		const ULONG nReferencesBefore = spDevice->Release();
		if (!CheckImage(spDevice.Get(), spContext.Get(), converter, 17, 13, 2.5f, nMaxError))
			return 1;
		converter.reset();
		spContext->Flush();
		spDevice->AddRef();
		const ULONG nReferencesAfter = spDevice->Release();
		if (nReferencesAfter != nReferencesBefore)
		{
			std::cerr << "Device reference growth after reset: " << nReferencesBefore
				<< " -> " << nReferencesAfter << '\n';
			return 1;
		}
	}
	// A different device must recreate every device-owned resource, not reuse
	// views or shaders from the previous device. This is an offscreen test only.
	ComPtr<ID3D11Device> spSecondDevice;
	ComPtr<ID3D11DeviceContext> spSecondContext;
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, nFlags, levels, 1,
		D3D11_SDK_VERSION, &spSecondDevice, nullptr, &spSecondContext);
	if (FAILED(hr)
		|| !CheckImage(spSecondDevice.Get(), spSecondContext.Get(), converter, 31, 7, 2.5f, nMaxError)
		|| !CheckImage(spDevice.Get(), spContext.Get(), converter, 19, 11, 2.5f, nMaxError))
		return 1;
	converter.reset();
	if (!KHdrCaptureTestAccess::CheckPath(spDevice.Get(), spContext.Get(), AutoHdrConversionMode, false)
		|| !KHdrCaptureTestAccess::CheckPath(spDevice.Get(), spContext.Get(), CpuHdrConversionMode, false))
		return 1;
	// FL10 cannot create the SM5 shaders: a genuine initialization failure
	// must fall back once, retain that decision for SDR, and clear it on stop.
	ComPtr<ID3D11Device> spLimitedDevice;
	ComPtr<ID3D11DeviceContext> spLimitedContext;
	const D3D_FEATURE_LEVEL limitedLevels[] = { D3D_FEATURE_LEVEL_10_0 };
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		limitedLevels, 1, D3D11_SDK_VERSION, &spLimitedDevice, nullptr, &spLimitedContext);
	if (FAILED(hr) || !KHdrCaptureTestAccess::CheckPath(spLimitedDevice.Get(), spLimitedContext.Get(),
		AutoHdrConversionMode, true))
		return 1;
#if defined(WRC_HDR_FAULT_TESTS)
	for (const char *pStage : { "shader", "srv", "rtv", "device" })
	{
		g_pFailureStage = pStage;
		g_nFailureHits = 0;
		const bool bDeviceLoss = std::strcmp(pStage, "device") == 0;
		if (!KHdrCaptureTestAccess::CheckPath(spDevice.Get(), spContext.Get(),
			AutoHdrConversionMode, !bDeviceLoss, bDeviceLoss)
			|| g_nFailureHits != (bDeviceLoss ? 2 : 1))
		{
			std::cerr << "Fault case failed: " << pStage << " hits=" << g_nFailureHits << '\n';
			return 1;
		}
		g_pFailureStage = nullptr;
		if (!KHdrCaptureTestAccess::CheckPath(spDevice.Get(), spContext.Get(), AutoHdrConversionMode, false))
			return 1;
		std::cout << "PASS injected " << pStage << " and subsequent GPU recovery\n";
	}
#endif
	if (spMessages)
	{
		for (UINT64 nIndex = 0; nIndex < spMessages->GetNumStoredMessages(); ++nIndex)
		{
			SIZE_T nSize = 0;
			spMessages->GetMessage(nIndex, nullptr, &nSize);
			std::vector<unsigned char> data(nSize);
			auto *pMessage = reinterpret_cast<D3D11_MESSAGE *>(data.data());
			spMessages->GetMessage(nIndex, pMessage, &nSize);
			if (pMessage->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
			{
				std::cerr << pMessage->pDescription << '\n';
				return 1;
			}
		}
	}
	std::cout << "PASS " << (bWarp ? "WARP" : "hardware") << " maxError=" << nMaxError
		<< " debugLayer=" << static_cast<bool>(spMessages) << " resetCycles=20\n";
	return 0;
}
