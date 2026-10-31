#include "capture/hdrgpubgraconverter.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
std::array<int, 3> Reference(const unsigned short *pPixel, double fWhite)
{
	std::array<double, 3> color;
	for (int nChannel = 0; nChannel < 3; ++nChannel)
	{
		double fValue = DirectX::PackedVector::XMConvertHalfToFloat(pPixel[nChannel]);
		if (std::isnan(fValue))
			fValue = 0;
		color[nChannel] = std::clamp(std::clamp(fValue, -65504.0, 65504.0) / fWhite, -65504.0, 65504.0);
	}
	const double fLuminance = std::max(0.0, color[0] * .2126 + color[1] * .7152 + color[2] * .0722);
	const double fMinimum = *std::min_element(color.begin(), color.end());
	if (fMinimum < 0)
	{
		const double fFactor = fLuminance / (fLuminance - fMinimum);
		for (double &fValue : color)
			fValue = std::max(0.0, fLuminance + (fValue - fLuminance) * fFactor);
	}
	const double fPeak = *std::max_element(color.begin(), color.end());
	const double fMapped = fPeak > .75 ? .75 + .25 * (fPeak - .75) / (fPeak - .5) : fPeak;
	std::array<int, 3> result;
	for (int nChannel = 0; nChannel < 3; ++nChannel)
	{
		const double fValue = std::clamp(color[nChannel] * (fPeak > 0 ? fMapped / fPeak : 1), 0.0, 1.0);
		const double fSrgb = fValue <= .0031308 ? 12.92 * fValue : 1.055 * std::pow(fValue, 1 / 2.4) - .055;
		result[nChannel] = static_cast<int>(fSrgb * 255 + .5);
	}
	return result;
}

bool Check(ID3D11Device *pDevice, ID3D11DeviceContext *pContext, KHdrGpuBgraConverter &converter,
	const std::vector<unsigned short> &pixels, UINT nWidth, UINT nHeight, float fWhite, int &nMaxError)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = nWidth;
	desc.Height = nHeight;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	D3D11_SUBRESOURCE_DATA data = { pixels.data(), nWidth * 8, 0 };
	ComPtr<ID3D11Texture2D> spInput, spOutput, spStaging;
	if (FAILED(pDevice->CreateTexture2D(&desc, &data, &spInput))
		|| FAILED(converter.convert(pDevice, pContext, spInput.Get(), fWhite, &spOutput)))
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
		const auto *pRow = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch;
		for (UINT x = 0; x < nWidth; ++x)
		{
			const auto expected = Reference(&pixels[(static_cast<size_t>(y) * nWidth + x) * 4], fWhite);
			for (int nChannel = 0; nChannel < 3; ++nChannel)
			{
				const int nError = std::abs(static_cast<int>(pRow[x * 4 + 2 - nChannel]) - expected[nChannel]);
				if (nError > 2 && nMaxError <= 2)
					std::cerr << "first mismatch x=" << x << " y=" << y << " channel=" << nChannel
						<< " actual=" << static_cast<int>(pRow[x * 4 + 2 - nChannel]) << " expected=" << expected[nChannel] << '\n';
				nMaxError = std::max(nMaxError, nError);
			}
			bValid &= pRow[x * 4 + 3] == 255;
		}
	}
	pContext->Unmap(spStaging.Get(), 0);
	return bValid && nMaxError <= 2;
}
}

int main(int nArgc, char **pArgv)
{
	const bool bWarp = nArgc > 1 && std::string(pArgv[1]) == "--warp";
	ComPtr<ID3D11Device> spDevice;
	ComPtr<ID3D11DeviceContext> spContext;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(nullptr, bWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
		&spDevice, nullptr, &spContext)))
		return 77;
	KHdrGpuBgraConverter converter;
	std::vector<unsigned short> pixels(257 * 256 * 4);
	for (size_t nIndex = 0; nIndex < pixels.size() / 4; ++nIndex)
	{
		pixels[nIndex * 4] = static_cast<unsigned short>(nIndex);
		pixels[nIndex * 4 + 1] = static_cast<unsigned short>(nIndex * 251 + 123);
		pixels[nIndex * 4 + 2] = static_cast<unsigned short>(65535 - nIndex);
	}
	int nMaxError = 0;
	pixels[0] = pixels[1] = pixels[2] = 0x3c00;
	for (float fWhite : { 1.f, 2.f, 2.5f, 4.f })
	{
		if (!Check(spDevice.Get(), spContext.Get(), converter, pixels, 257, 256, fWhite, nMaxError))
		{
			std::cerr << "reference mismatch maxError=" << nMaxError << '\n';
			return 1;
		}
		std::vector<unsigned short> gray;
		for (float fLevel : { 0.f, .003f, .018f, .18f, .5f, .75f, 1.f, 1.25f, 1.5f, 2.f, 3.f, 4.f })
		{
			const auto nBits = DirectX::PackedVector::XMConvertFloatToHalf(fLevel * fWhite);
			gray.insert(gray.end(), { nBits, nBits, nBits, 0 });
		}
		if (!Check(spDevice.Get(), spContext.Get(), converter, gray, 12, 1, fWhite, nMaxError))
			return 1;
		if (std::abs(Reference(&gray[3 * 4], fWhite)[0] - 118) > 12
			|| Reference(&gray[6 * 4], fWhite)[0] < 230)
			return 1;
		int nDistinct = 0;
		int nPrevious = Reference(&gray[6 * 4], fWhite)[0];
		for (size_t nIndex = 7; nIndex < 12; ++nIndex)
		{
			const int nValue = Reference(&gray[nIndex * 4], fWhite)[0];
			if (nValue < nPrevious)
				return 1;
			nDistinct += nValue > nPrevious ? 1 : 0;
			nPrevious = nValue;
		}
		if (nDistinct < 4)
			return 1;
		std::cout << "whiteNits=" << fWhite * 80 << " grayCodes=";
		for (size_t nIndex = 0; nIndex < 12; ++nIndex)
			std::cout << Reference(&gray[nIndex * 4], fWhite)[0] << ',';
		std::cout << '\n';
		converter.reset();
	}
	std::cout << "PASS desktop mapping all FP16 bit patterns, mixed RGB, odd dimensions, reset; maxError="
		<< nMaxError << '\n';
	return 0;
}
