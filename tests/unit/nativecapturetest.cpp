#include "capture/dxgidesktopduplicator.h"

#include <QtCore/QCoreApplication>

#include <array>
#include <iostream>
#include <cmath>

// Synthetic textures only: do not initialize or acquire DXGI duplication.
struct KHdrCaptureTestAccess
{
	static void setDevice(KDxgiDesktopDuplicator *pCapture, ID3D11Device *pDevice, ID3D11DeviceContext *pContext)
	{
		pCapture->m_spDevice = pDevice;
		pCapture->m_spContext = pContext;
		pCapture->m_nAdapterLuid = 123;
	}
	static IKCaptureSource::CaptureResult copy(KDxgiDesktopDuplicator *pCapture,
		ID3D11Texture2D *pSource, KCaptureFrame *pFrame, QString *pError)
	{
		return pCapture->copyTextureToNativeFrame(pSource, pFrame, pError);
	}
};

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	Microsoft::WRL::ComPtr<ID3D11Device> spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> spContext;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		nullptr, 0, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext)))
		return 77;
	auto spPath = std::make_shared<KVideoFramePathState>(GpuVideoFramePath);
	spPath->begin(1);
	spPath->setSeparateCursor(1, true);
	KDxgiDesktopDuplicator capture(AutoHdrConversionMode, spPath);
	KHdrCaptureTestAccess::setDevice(&capture, spDevice.Get(), spContext.Get());
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = 64;
	descriptor.Height = 48;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spSource;
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> spView;
	if (FAILED(spDevice->CreateTexture2D(&descriptor, nullptr, &spSource))
		|| FAILED(spDevice->CreateRenderTargetView(spSource.Get(), nullptr, &spView)))
		return 1;
	KD3D11BgraFrame empty(nullptr, std::make_shared<std::recursive_mutex>(), 1, 123);
	std::vector<unsigned char> invalidPixels;
	if (empty.texture() || empty.width() != 0 || empty.height() != 0
		|| SUCCEEDED(empty.copyBgra(&invalidPixels)))
		return 1;
	for (int nCase = 0; nCase < 2; ++nCase)
	{
		D3D11_TEXTURE2D_DESC invalidDescriptor = descriptor;
		invalidDescriptor.BindFlags = 0;
		if (nCase == 0)
			invalidDescriptor.ArraySize = 2;
		else
			invalidDescriptor.MipLevels = 2;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> spInvalid;
		if (FAILED(spDevice->CreateTexture2D(&invalidDescriptor, nullptr, &spInvalid)))
			return 1;
		KD3D11BgraFrame unsupported(spInvalid.Get(), std::make_shared<std::recursive_mutex>(), 1, 123);
		if (unsupported.texture() || unsupported.width() != 0 || unsupported.height() != 0
			|| SUCCEEDED(unsupported.copyBgra(&invalidPixels)))
			return 1;
	}
	std::array<KCaptureFrame, 4> held;
	QString strError;
	for (int nIndex = 0; nIndex < 4; ++nIndex)
	{
		const float color[4] = {(nIndex + 1) * 0.2f, 0, 0, 1};
		spContext->ClearRenderTargetView(spView.Get(), color);
		if (KHdrCaptureTestAccess::copy(&capture, spSource.Get(), &held[nIndex], &strError)
			!= IKCaptureSource::CapturedCaptureResult || !held[nIndex].spNativeFrame
			|| !held[nIndex].vecBgraBuffer.empty())
			return 1;
	}
	KCaptureFrame extra;
	if (KHdrCaptureTestAccess::copy(&capture, spSource.Get(), &extra, &strError)
		!= IKCaptureSource::TimeoutCaptureResult)
		return 1;
	held[0] = {};
	if (KHdrCaptureTestAccess::copy(&capture, spSource.Get(), &extra, &strError)
		!= IKCaptureSource::CapturedCaptureResult)
		return 1;
	spPath->begin(2);
	spPath->setSeparateCursor(2, true);
	KCaptureFrame next;
	if (KHdrCaptureTestAccess::copy(&capture, spSource.Get(), &next, &strError)
		!= IKCaptureSource::CapturedCaptureResult || next.spNativeFrame->generation() != 2)
		return 1;
	for (int nIndex = 1; nIndex < 4; ++nIndex)
	{
		auto *pFrame = static_cast<KD3D11BgraFrame *>(held[nIndex].spNativeFrame.get());
		std::vector<unsigned char> pixels;
		if (pFrame->generation() != 1 || FAILED(pFrame->copyBgra(&pixels))
			|| std::abs(static_cast<int>(pixels[2]) - (nIndex + 1) * 51) > 1)
			return 1;
	}
	// HDR source: red at SDR white, green at zero, blue above white. Conversion
	// must remain on GPU and produce encoded SDR BGRA before NV12 conversion.
	descriptor.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	descriptor.BindFlags = 0;
	std::vector<unsigned short> hdrPixels(64 * 48 * 4);
	for (size_t nIndex = 0; nIndex < hdrPixels.size(); nIndex += 4)
	{
		hdrPixels[nIndex] = 0x4100; // 2.5
		hdrPixels[nIndex + 1] = 0;
		hdrPixels[nIndex + 2] = 0x4400; // 4.0
		hdrPixels[nIndex + 3] = 0x3c00; // 1.0
	}
	D3D11_SUBRESOURCE_DATA initial = {hdrPixels.data(), 64 * 8, 0};
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spHdr;
	if (FAILED(spDevice->CreateTexture2D(&descriptor, &initial, &spHdr)))
		return 1;
	KCaptureFrame hdr;
	if (KHdrCaptureTestAccess::copy(&capture, spHdr.Get(), &hdr, &strError)
		!= IKCaptureSource::CapturedCaptureResult || !hdr.vecBgraBuffer.empty())
		return 1;
	std::vector<unsigned char> pixels;
	if (FAILED(static_cast<KD3D11BgraFrame *>(hdr.spNativeFrame.get())->copyBgra(&pixels))
		|| pixels[0] != 255 || pixels[1] != 0 || pixels[2] != 255 || pixels[3] != 255)
		return 1;
	// Alternate physical source dimensions while the preceding frame remains
	// owned by a consumer. Rebuilding the active pool must not invalidate it.
	for (int nCycle = 0; nCycle < 20; ++nCycle)
	{
		const int nWidth = nCycle % 2 == 0 ? 80 : 64;
		const int nHeight = nCycle % 2 == 0 ? 50 : 48;
		descriptor.Width = nWidth;
		descriptor.Height = nHeight;
		descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		std::vector<unsigned char> sourcePixels(static_cast<size_t>(nWidth) * nHeight * 4, 51);
		D3D11_SUBRESOURCE_DATA resizedInitial = {sourcePixels.data(), static_cast<UINT>(nWidth * 4), 0};
		Microsoft::WRL::ComPtr<ID3D11Texture2D> spResizedSource;
		if (FAILED(spDevice->CreateTexture2D(&descriptor, &resizedInitial, &spResizedSource)))
			return 1;
		KCaptureFrame previous = next;
		if (KHdrCaptureTestAccess::copy(&capture, spResizedSource.Get(), &next, &strError)
			!= IKCaptureSource::CapturedCaptureResult || next.nWidth != nWidth || next.nHeight != nHeight
			|| next.spNativeFrame->generation() != 2 || !next.vecBgraBuffer.empty())
			return 1;
		std::vector<unsigned char> previousPixels;
		if (FAILED(static_cast<KD3D11BgraFrame *>(previous.spNativeFrame.get())->copyBgra(&previousPixels))
			|| previousPixels.size() != static_cast<size_t>(previous.nWidth) * previous.nHeight * 4)
			return 1;
		if (nCycle > 0 && previousPixels[0] != 51)
			return 1;
	}
	capture.shutdown();
	if (FAILED(static_cast<KD3D11BgraFrame *>(next.spNativeFrame.get())->copyBgra(&pixels))
		|| pixels[0] != 51)
		return 1;

	// An unsupported synthetic format follows the real initialization error
	// boundary; it is not a forced driver reset or an actual device-loss test.
	descriptor.Width = 64;
	descriptor.Height = 48;
	descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spUnsupported;
	if (FAILED(spDevice->CreateTexture2D(&descriptor, nullptr, &spUnsupported)))
		return 1;
	for (int nCase = 0; nCase < 3; ++nCase)
	{
		auto spFailurePath = std::make_shared<KVideoFramePathState>(
			nCase == 2 ? GpuVideoFramePath : AutoVideoFramePath);
		spFailurePath->begin(10);
		if (nCase == 1)
			spFailurePath->selectGpu(10);
		KDxgiDesktopDuplicator failing(AutoHdrConversionMode, spFailurePath);
		KHdrCaptureTestAccess::setDevice(&failing, spDevice.Get(), spContext.Get());
		KCaptureFrame rejected;
		strError.clear();
		if (KHdrCaptureTestAccess::copy(&failing, spUnsupported.Get(), &rejected, &strError)
			!= IKCaptureSource::ErrorCaptureResult || strError.isEmpty() || rejected.spNativeFrame)
			return 1;
		const auto expected = nCase == 0 ? KVideoFramePathState::Cpu : KVideoFramePathState::Failed;
		if (spFailurePath->snapshot().decision != expected)
			return 1;
		spFailurePath->begin(11);
		spFailurePath->setSeparateCursor(11, true);
		if (KHdrCaptureTestAccess::copy(&failing, spSource.Get(), &rejected, &strError)
			!= IKCaptureSource::CapturedCaptureResult || rejected.spNativeFrame->generation() != 11)
			return 1;
	}
	std::cout << "native capture: null/array/mip rejection, bounded pool, ownership, generation, GPU HDR, 20 resizes, "
		"late release after shutdown, initial fallback and runtime/forced failure passed; no desktop acquired"
		<< std::endl;
	return 0;
}
