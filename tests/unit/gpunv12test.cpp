#include "capture/gpunv12converter.h"

extern "C"
{
#include <libswscale/swscale.h>
}

#include <QtCore/QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
bool Check(HRESULT hr, const char *pOperation)
{
	if (SUCCEEDED(hr))
		return true;
	std::cerr << pOperation << " HRESULT=" << std::hex << hr << std::dec << std::endl;
	return false;
}

struct KRgb
{
	double fRed = 0;
	double fGreen = 0;
	double fBlue = 0;
};

KRgb Sample(const std::vector<unsigned char> &pixels, int nStride, int nWidth, int nHeight,
	double fX, double fY)
{
	fX = std::clamp(fX, 0.0, static_cast<double>(nWidth - 1));
	fY = std::clamp(fY, 0.0, static_cast<double>(nHeight - 1));
	const int nLeft = static_cast<int>(std::floor(fX));
	const int nTop = static_cast<int>(std::floor(fY));
	const int nRight = (std::min)(nLeft + 1, nWidth - 1);
	const int nBottom = (std::min)(nTop + 1, nHeight - 1);
	const double fHorizontal = fX - nLeft;
	const double fVertical = fY - nTop;
	std::array<double, 3> channels{};
	for (int nChannel = 0; nChannel < 3; ++nChannel)
	{
		const double fUpper = pixels[nTop * nStride + nLeft * 4 + nChannel] * (1 - fHorizontal)
			+ pixels[nTop * nStride + nRight * 4 + nChannel] * fHorizontal;
		const double fLower = pixels[nBottom * nStride + nLeft * 4 + nChannel] * (1 - fHorizontal)
			+ pixels[nBottom * nStride + nRight * 4 + nChannel] * fHorizontal;
		channels[nChannel] = (fUpper * (1 - fVertical) + fLower * fVertical) / 255;
	}
	return {channels[2], channels[1], channels[0]};
}

bool TestConversion(ID3D11Device *pDevice, ID3D11DeviceContext *pContext,
	KGpuNv12Converter *pConverter, int nSourceWidth, int nSourceHeight, int nWidth, int nHeight, int nPattern)
{
	const int nStride = nSourceWidth * 4 + 20;
	std::vector<unsigned char> pixels(nStride * nSourceHeight, 0xcd);
	std::uint32_t nRandom = 0x71837a;
	for (int y = 0; y < nSourceHeight; ++y)
	{
		for (int x = 0; x < nSourceWidth; ++x)
		{
			for (int nChannel = 0; nChannel < 3; ++nChannel)
			{
				nRandom = nRandom * 1664525u + 1013904223u;
				unsigned char nValue = static_cast<unsigned char>(nRandom >> 24);
				if (nPattern == 0)
					nValue = static_cast<unsigned char>((x * 255) / (nSourceWidth - 1));
				else if (nPattern == 1)
					nValue = ((x / 8) >> nChannel) & 1 ? 255 : 0;
				pixels[y * nStride + x * 4 + nChannel] = nValue;
			}
			pixels[y * nStride + x * 4 + 3] = 255;
		}
	}
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = nSourceWidth;
	descriptor.Height = nSourceHeight;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = nPattern == 1 ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_UNORM;
	descriptor.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA initial = {pixels.data(), static_cast<UINT>(nStride), 0};
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spSource;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spOutput;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> spStaging;
	if (!Check(pDevice->CreateTexture2D(&descriptor, &initial, &spSource), "create source"))
		return false;
	descriptor.Width = (nWidth + 15) & ~15;
	descriptor.Height = (nHeight + 15) & ~15;
	descriptor.Format = DXGI_FORMAT_NV12;
	descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
	if (!Check(pDevice->CreateTexture2D(&descriptor, nullptr, &spOutput), "create NV12"))
		return false;
	descriptor.Usage = D3D11_USAGE_STAGING;
	descriptor.BindFlags = 0;
	descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (!Check(pDevice->CreateTexture2D(&descriptor, nullptr, &spStaging), "create readback")
		|| !Check(pConverter->convert(pContext, spSource.Get(), spOutput.Get(), nWidth, nHeight), "convert"))
		return false;
	if (pConverter->convert(pContext, spSource.Get(), spOutput.Get(), nWidth - 1, nHeight) != E_INVALIDARG
		|| pConverter->convert(pContext, spSource.Get(), spOutput.Get(), descriptor.Width + 2, nHeight) != E_INVALIDARG)
		return false;
	pContext->CopyResource(spStaging.Get(), spOutput.Get());
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (!Check(pContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "read converted NV12"))
		return false;
	int nMaxY = 0, nMaxUv = 0;
	bool bPaddingValid = true;
	const auto *pData = static_cast<const unsigned char *>(mapped.pData);
	const auto *pUv = pData + mapped.RowPitch * descriptor.Height;
	auto sampleOutput = [&](int x, int y)
	{
		return Sample(pixels, nStride, nSourceWidth, nSourceHeight,
			(x + 0.5) * nSourceWidth / nWidth - 0.5, (y + 0.5) * nSourceHeight / nHeight - 0.5);
	};
	for (UINT y = 0; y < descriptor.Height; ++y)
	{
		for (UINT x = 0; x < descriptor.Width; ++x)
		{
			if (y >= static_cast<UINT>(nHeight) || x >= static_cast<UINT>(nWidth))
			{
				bPaddingValid = bPaddingValid && pData[y * mapped.RowPitch + x] == 16;
				continue;
			}
			const KRgb color = sampleOutput(x, y);
			const int nExpected = static_cast<int>(std::lround(16 + 65.481 * color.fRed
				+ 128.553 * color.fGreen + 24.966 * color.fBlue));
			nMaxY = (std::max)(nMaxY, std::abs(nExpected - pData[y * mapped.RowPitch + x]));
		}
	}
	for (UINT y = 0; y < descriptor.Height / 2; ++y)
	{
		for (UINT x = 0; x < descriptor.Width / 2; ++x)
		{
			if (y >= static_cast<UINT>(nHeight / 2) || x >= static_cast<UINT>(nWidth / 2))
			{
				bPaddingValid = bPaddingValid && pUv[y * mapped.RowPitch + 2 * x] == 128
					&& pUv[y * mapped.RowPitch + 2 * x + 1] == 128;
				continue;
			}
			KRgb average;
			for (int nOffsetY = 0; nOffsetY < 2; ++nOffsetY)
			{
				for (int nOffsetX = 0; nOffsetX < 2; ++nOffsetX)
				{
					const KRgb color = sampleOutput(2 * x + nOffsetX, 2 * y + nOffsetY);
					average.fRed += color.fRed / 4;
					average.fGreen += color.fGreen / 4;
					average.fBlue += color.fBlue / 4;
				}
			}
			const int nU = static_cast<int>(std::lround(128 - 37.797 * average.fRed
				- 74.203 * average.fGreen + 112 * average.fBlue));
			const int nV = static_cast<int>(std::lround(128 + 112 * average.fRed
				- 93.786 * average.fGreen - 18.214 * average.fBlue));
			nMaxUv = (std::max)({nMaxUv, std::abs(nU - pUv[y * mapped.RowPitch + 2 * x]),
				std::abs(nV - pUv[y * mapped.RowPitch + 2 * x + 1])});
		}
	}
	// -1 means the scaling reference is the explicit scalar sampler, not swscale.
	int nSwscaleMaxY = -1, nSwscaleMaxUv = -1;
	if (nSourceWidth == nWidth && nSourceHeight == nHeight)
	{
		nSwscaleMaxY = nSwscaleMaxUv = 0;
		SwsContext *pSws = sws_getContext(nSourceWidth, nSourceHeight, AV_PIX_FMT_BGRA,
			nWidth, nHeight, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		std::vector<unsigned char> yPlane(nWidth * nHeight);
		std::vector<unsigned char> uPlane(nWidth * nHeight / 4);
		std::vector<unsigned char> vPlane(nWidth * nHeight / 4);
		const unsigned char *sourceData[4] = {pixels.data(), nullptr, nullptr, nullptr};
		const int sourceStride[4] = {nStride, 0, 0, 0};
		unsigned char *destinationData[4] = {yPlane.data(), uPlane.data(), vPlane.data(), nullptr};
		const int destinationStride[4] = {nWidth, nWidth / 2, nWidth / 2, 0};
		if (!pSws || sws_scale(pSws, sourceData, sourceStride, 0, nSourceHeight,
			destinationData, destinationStride) != nHeight)
		{
			bPaddingValid = false;
		}
		else
		{
			for (int y = 0; y < nHeight; ++y)
			{
				for (int x = 0; x < nWidth; ++x)
					nSwscaleMaxY = (std::max)(nSwscaleMaxY,
						std::abs(yPlane[y * nWidth + x] - pData[y * mapped.RowPitch + x]));
			}
			for (int y = 0; y < nHeight / 2; ++y)
			{
				for (int x = 0; x < nWidth / 2; ++x)
				{
					nSwscaleMaxUv = (std::max)({nSwscaleMaxUv,
						std::abs(uPlane[y * nWidth / 2 + x] - pUv[y * mapped.RowPitch + x * 2]),
						std::abs(vPlane[y * nWidth / 2 + x] - pUv[y * mapped.RowPitch + x * 2 + 1])});
				}
			}
		}
		sws_freeContext(pSws);
	}
	pContext->Unmap(spStaging.Get(), 0);
	std::cout << "source=" << nSourceWidth << 'x' << nSourceHeight << " output=" << nWidth << 'x' << nHeight
		<< " pattern=" << nPattern << " maxY=" << nMaxY << " maxUV=" << nMaxUv
		<< " paddingValid=" << bPaddingValid << " swscaleMaxY=" << nSwscaleMaxY
		<< " swscaleMaxUV=" << nSwscaleMaxUv << std::endl;
	return nMaxY <= 3 && nMaxUv <= 3 && nSwscaleMaxY <= 3 && nSwscaleMaxUv <= 3 && bPaddingValid;
}

bool TestSwscaleDefaults()
{
	SwsContext *pContext = sws_getContext(64, 48, AV_PIX_FMT_BGRA, 64, 48,
		AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!pContext)
		return false;
	int *pInverse = nullptr, *pTable = nullptr;
	int nSourceRange = 0, nDestinationRange = 0, nBrightness = 0, nContrast = 0, nSaturation = 0;
	const int nResult = sws_getColorspaceDetails(pContext, &pInverse, &nSourceRange, &pTable,
		&nDestinationRange, &nBrightness, &nContrast, &nSaturation);
	const int *pReference = sws_getCoefficients(SWS_CS_ITU601);
	bool bMatch = nResult >= 0 && nSourceRange == 1 && nDestinationRange == 0;
	for (int nIndex = 0; bMatch && nIndex < 4; ++nIndex)
		bMatch = pTable[nIndex] == pReference[nIndex];
	std::cout << "swscaleDefault601=" << bMatch << " srcRange=" << nSourceRange
		<< " dstRange=" << nDestinationRange << std::endl;
	sws_freeContext(pContext);
	return bMatch;
}
}

int main(int nArgc, char **pArgv)
{
	QCoreApplication application(nArgc, pArgv);
	Microsoft::WRL::ComPtr<ID3D11Device> spDevice;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> spContext;
	const bool bWarp = application.arguments().contains(QStringLiteral("--warp"));
	const HRESULT hr = D3D11CreateDevice(nullptr, bWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext);
	if (!Check(hr, "create D3D11 device"))
		return 77;
	if (!TestSwscaleDefaults())
		return 1;
	KGpuNv12Converter converter;
	for (int nPattern = 0; nPattern < 3; ++nPattern)
	{
		for (const auto &dimensions : {std::array<int, 4>{64, 48, 64, 48}, {64, 48, 128, 96},
			{64, 48, 32, 24}, {65, 49, 62, 46}})
		{
			if (!TestConversion(spDevice.Get(), spContext.Get(), &converter,
				dimensions[0], dimensions[1], dimensions[2], dimensions[3], nPattern))
				return 1;
		}
	}
	converter.reset();
	return 0;
}
