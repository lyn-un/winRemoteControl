#include "capture/hdrtonemapper.h"
#include "capture/hdrgpubgraconverter.h"

#include <DirectXPackedVector.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int main(int nArgc, char **pArgv)
{
	const bool bWarp = nArgc > 1 && std::string(pArgv[1]) == "--warp";
	ComPtr<ID3D11Device> spDevice;
	ComPtr<ID3D11DeviceContext> spContext;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice(nullptr, bWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
		&spDevice, nullptr, &spContext);
	if (FAILED(hr))
		return 77;
	KHdrToneMapper mapper;
	KHdrGpuBgraConverter converter;
	converter.setDesktopToneMapping(false);
	if (nArgc > 1 && std::string(pArgv[1]) == "--resources")
	{
		SIZE_T nBaseline = 0;
		for (int nCycle = 1; nCycle <= 120; ++nCycle)
		{
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = nCycle % 2 ? 1920 : 2560;
			desc.Height = nCycle % 2 ? 1080 : 1440;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			std::vector<unsigned short> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4, 0x3c00);
			D3D11_SUBRESOURCE_DATA data = { pixels.data(), desc.Width * 8, 0 };
			ComPtr<ID3D11Texture2D> spInput, spLinear, spOutput, spStaging;
			if (FAILED(spDevice->CreateTexture2D(&desc, &data, &spInput))
				|| FAILED(mapper.convert(spDevice.Get(), spContext.Get(), spInput.Get(), 2.5f, 1000, &spLinear))
				|| FAILED(converter.convert(spDevice.Get(), spContext.Get(), spLinear.Get(), 2.5f, &spOutput)))
				return 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(spDevice->CreateTexture2D(&desc, nullptr, &spStaging)))
				return 1;
			spContext->CopyResource(spStaging.Get(), spOutput.Get());
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (FAILED(spContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return 1;
			spContext->Unmap(spStaging.Get(), 0);
			spInput.Reset();
			spLinear.Reset();
			spOutput.Reset();
			spStaging.Reset();
			mapper.reset();
			converter.reset();
			spContext->Flush();
			PROCESS_MEMORY_COUNTERS_EX memory = {};
			if (!GetProcessMemoryInfo(GetCurrentProcess(),
				reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
				return 1;
			if (nCycle == 20)
				nBaseline = memory.PrivateUsage;
			if (nCycle > 20 && memory.PrivateUsage > nBaseline + 32 * 1024 * 1024)
				return 1;
			if (nCycle % 20 == 0)
				std::cout << "cycle=" << nCycle << " privateBytes=" << memory.PrivateUsage << '\n';
		}
		return 0;
	}
	ComPtr<ID3D11Texture2D> spInvalid;
	if (mapper.convert(nullptr, nullptr, nullptr, 0, 0, &spInvalid) != E_INVALIDARG
		|| mapper.convert(nullptr, nullptr, nullptr, 1, 1000, nullptr) != E_POINTER)
		return 1;
	bool bAccept = true;
	// Freeze desktop acceptance before evaluating: 18% gray error <=12 code values,
	// white >=230, >=4 distinct super-white steps. Failure rejects default enablement.
	for (float fWhite : { 1.0f, 2.0f, 2.5f, 4.0f })
	{
		for (float fPeak : { 1000.0f, 2000.0f, 4000.0f })
		{
			const float levels[] = { 0, 0.003f, 0.018f, 0.18f, 0.5f, 0.75f, 1,
				1.25f, 1.5f, 2, 3, 4 };
			std::vector<DirectX::PackedVector::HALF> pixels;
			for (float fLevel : levels)
			{
				for (int nChannel = 0; nChannel < 3; ++nChannel)
					pixels.push_back(DirectX::PackedVector::XMConvertFloatToHalf(fLevel * fWhite));
				pixels.push_back(DirectX::PackedVector::XMConvertFloatToHalf(1));
			}
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = 12;
			desc.Height = 1;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			D3D11_SUBRESOURCE_DATA data = { pixels.data(), 12 * 8, 0 };
			ComPtr<ID3D11Texture2D> spInput, spLinear, spOutput, spStaging;
			hr = spDevice->CreateTexture2D(&desc, &data, &spInput);
			if (SUCCEEDED(hr))
				hr = mapper.convert(spDevice.Get(), spContext.Get(), spInput.Get(), fWhite, fPeak, &spLinear);
			if (SUCCEEDED(hr))
				hr = converter.convert(spDevice.Get(), spContext.Get(), spLinear.Get(), fWhite, &spOutput);
			if (FAILED(hr))
			{
				std::cerr << "candidate_error=0x" << std::hex << hr << '\n';
				return 1;
			}
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(spDevice->CreateTexture2D(&desc, nullptr, &spStaging)))
				return 1;
			spContext->CopyResource(spStaging.Get(), spOutput.Get());
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (FAILED(spContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return 1;
			const auto *pBytes = static_cast<const unsigned char *>(mapped.pData);
			int nPrevious = -1;
			int nDistinct = 0;
			bool bNeutralMonotonic = true;
			std::cout << "white=" << fWhite * 80 << " peakPolicy=" << fPeak << " grayCodes=";
			for (int nIndex = 0; nIndex < 12; ++nIndex)
			{
				const int nValue = pBytes[nIndex * 4];
				bNeutralMonotonic &= nValue >= nPrevious && pBytes[nIndex * 4 + 1] == nValue
					&& pBytes[nIndex * 4 + 2] == nValue && pBytes[nIndex * 4 + 3] == 255;
				if (nIndex >= 7 && nValue != nPrevious)
					++nDistinct;
				nPrevious = nValue;
				std::cout << nValue << ',';
			}
			const bool bDesktop = std::abs(static_cast<int>(pBytes[3 * 4]) - 118) <= 12
				&& pBytes[6 * 4] >= 230 && nDistinct >= 4;
			std::cout << " desktopAccepted=" << bDesktop << " neutralMonotonic=" << bNeutralMonotonic << '\n';
			spContext->Unmap(spStaging.Get(), 0);
			if (!bNeutralMonotonic)
				return 1;
			bAccept &= bDesktop;
		}
	}
	std::cout << "candidateDefaultAccepted=" << bAccept << '\n';
	// Evaluation executable reports a rejected candidate distinctly, not a broken old renderer.
	return bAccept ? 0 : 2;
}
