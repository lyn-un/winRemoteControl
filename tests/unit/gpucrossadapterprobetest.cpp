// Synthetic resources only. No desktop duplication, network or input injection.
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QScopeGuard>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kWidth = 64;
constexpr UINT kHeight = 48;

struct KDevice
{
	ComPtr<ID3D11Device> spDevice;
	ComPtr<ID3D11Device1> spDevice1;
	ComPtr<ID3D11DeviceContext> spContext;
	DXGI_ADAPTER_DESC1 descriptor = {};
	bool initialize(IDXGIAdapter1 *pAdapter)
	{
		return SUCCEEDED(pAdapter->GetDesc1(&descriptor))
			&& SUCCEEDED(D3D11CreateDevice(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
				D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
				nullptr, 0, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext))
			&& SUCCEEDED(spDevice.As(&spDevice1));
	}
};

struct KSharedHandle
{
	HANDLE hValue = nullptr;
	KSharedHandle() = default;
	KSharedHandle(const KSharedHandle &) = delete;
	KSharedHandle &operator=(const KSharedHandle &) = delete;
	~KSharedHandle()
	{
		if (hValue)
			CloseHandle(hValue);
	}
};

struct KProbeResult
{
	QString strStage;
	HRESULT hr = S_OK;
	bool bOpened = false;
	bool bVerified = false;
};

KProbeResult Probe(const KDevice &source, const KDevice &target, DXGI_FORMAT format)
{
	KProbeResult result;
	const UINT nRowBytes = kWidth * (format == DXGI_FORMAT_NV12 ? 1 : 4);
	const UINT nRows = format == DXGI_FORMAT_NV12 ? kHeight * 3 / 2 : kHeight;
	std::vector<unsigned char> pixels(static_cast<size_t>(nRowBytes) * nRows, 0x60);
	if (format == DXGI_FORMAT_NV12)
		std::fill(pixels.begin() + static_cast<size_t>(nRowBytes) * kHeight, pixels.end(), 0x80);
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = kWidth;
	descriptor.Height = kHeight;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = format;
	descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
	descriptor.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	D3D11_SUBRESOURCE_DATA initial = {pixels.data(), nRowBytes, 0};
	ComPtr<ID3D11Texture2D> spSource;
	result.strStage = QStringLiteral("create_shared_render_target");
	result.hr = source.spDevice->CreateTexture2D(&descriptor, &initial, &spSource);
	if (FAILED(result.hr))
		return result;
	ComPtr<IDXGIResource1> spResource;
	ComPtr<IDXGIKeyedMutex> spSourceMutex;
	result.strStage = QStringLiteral("query_shared_interfaces");
	result.hr = spSource.As(&spResource);
	if (SUCCEEDED(result.hr))
		result.hr = spSource.As(&spSourceMutex);
	if (FAILED(result.hr))
		return result;
	KSharedHandle handle;
	result.strStage = QStringLiteral("create_nt_handle");
	result.hr = spResource->CreateSharedHandle(nullptr,
		DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle.hValue);
	if (FAILED(result.hr))
		return result;
	result.strStage = QStringLiteral("source_acquire");
	result.hr = spSourceMutex->AcquireSync(0, 500);
	// WAIT_TIMEOUT/WAIT_ABANDONED are nonnegative; SUCCEEDED is insufficient.
	if (result.hr != S_OK)
		return result;
	source.spContext->Flush();
	result.strStage = QStringLiteral("source_release");
	result.hr = spSourceMutex->ReleaseSync(1);
	if (FAILED(result.hr))
		return result;
	ComPtr<ID3D11Texture2D> spShared;
	result.strStage = QStringLiteral("open_on_target");
	result.hr = target.spDevice1->OpenSharedResource1(handle.hValue, IID_PPV_ARGS(&spShared));
	if (FAILED(result.hr))
		return result;
	result.bOpened = true;
	ComPtr<IDXGIKeyedMutex> spTargetMutex;
	result.strStage = QStringLiteral("target_query_mutex");
	result.hr = spShared.As(&spTargetMutex);
	if (FAILED(result.hr))
		return result;
	result.strStage = QStringLiteral("target_acquire");
	result.hr = spTargetMutex->AcquireSync(1, 500);
	if (result.hr != S_OK)
		return result;
	const auto mutexGuard = qScopeGuard([&spTargetMutex]() { spTargetMutex->ReleaseSync(0); });
	descriptor.Usage = D3D11_USAGE_STAGING;
	descriptor.BindFlags = descriptor.MiscFlags = 0;
	descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> spStaging;
	result.strStage = QStringLiteral("target_staging");
	result.hr = target.spDevice->CreateTexture2D(&descriptor, nullptr, &spStaging);
	if (FAILED(result.hr))
		return result;
	target.spContext->CopyResource(spStaging.Get(), spShared.Get());
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	result.strStage = QStringLiteral("target_readback");
	result.hr = target.spContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(result.hr))
		return result;
	const auto mapGuard = qScopeGuard([&target, &spStaging]() { target.spContext->Unmap(spStaging.Get(), 0); });
	result.strStage = QStringLiteral("verify_bytes");
	result.hr = E_FAIL;
	if (mapped.RowPitch < nRowBytes)
		return result;
	for (UINT nRow = 0; nRow < nRows; ++nRow)
	{
		if (std::memcmp(static_cast<const unsigned char *>(mapped.pData) + static_cast<size_t>(nRow) * mapped.RowPitch,
			pixels.data() + static_cast<size_t>(nRow) * nRowBytes, nRowBytes) != 0)
		{
			return result;
		}
	}
	result.hr = target.spDevice->GetDeviceRemovedReason();
	result.bVerified = result.hr == S_OK;
	return result;
}

quint64 AdapterLuid(const KDevice &device)
{
	return (static_cast<quint64>(static_cast<quint32>(device.descriptor.AdapterLuid.HighPart)) << 32)
		| device.descriptor.AdapterLuid.LowPart;
}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	if (qEnvironmentVariableIntValue("WRC_RUN_GPU_FRAME_TEST") != 1)
		return 77;
	ComPtr<IDXGIFactory1> spFactory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&spFactory))))
		return 1;
	std::vector<ComPtr<IDXGIAdapter1>> adapters;
	LUID firstLuid = {};
	for (UINT nIndex = 0; nIndex < 16 && adapters.size() < 2; ++nIndex)
	{
		ComPtr<IDXGIAdapter1> spAdapter;
		const HRESULT hr = spFactory->EnumAdapters1(nIndex, &spAdapter);
		if (hr == DXGI_ERROR_NOT_FOUND)
			break;
		if (FAILED(hr))
			return 1;
		DXGI_ADAPTER_DESC1 descriptor = {};
		if (FAILED(spAdapter->GetDesc1(&descriptor)))
			return 1;
		if (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			continue;
		if (!adapters.empty() && descriptor.AdapterLuid.HighPart == firstLuid.HighPart
			&& descriptor.AdapterLuid.LowPart == firstLuid.LowPart)
		{
			continue;
		}
		adapters.push_back(spAdapter);
		if (adapters.size() == 1)
			firstLuid = descriptor.AdapterLuid;
	}
	if (adapters.size() < 2)
		return 77;
	std::array<KDevice, 2> devices;
	std::array<KDevice, 2> siblings;
	for (int nIndex = 0; nIndex < 2; ++nIndex)
	{
		if (!devices[nIndex].initialize(adapters[nIndex].Get())
			|| !siblings[nIndex].initialize(adapters[nIndex].Get()))
		{
			return 1;
		}
	}
	QJsonArray cases;
	bool bValid = true;
	for (int nSource = 0; nSource < 2; ++nSource)
	{
		for (DXGI_FORMAT format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12})
		{
			for (bool bCross : {false, true})
			{
				const KDevice &target = bCross ? devices[1 - nSource] : siblings[nSource];
				const auto result = Probe(devices[nSource], target, format);
				QJsonObject item;
				item[QStringLiteral("source")] = QString::fromWCharArray(devices[nSource].descriptor.Description);
				item[QStringLiteral("target")] = QString::fromWCharArray(target.descriptor.Description);
				item[QStringLiteral("sourceLuid")] = QString::number(AdapterLuid(devices[nSource]));
				item[QStringLiteral("targetLuid")] = QString::number(AdapterLuid(target));
				item[QStringLiteral("crossAdapter")] = bCross;
				item[QStringLiteral("format")] = format == DXGI_FORMAT_NV12 ? QStringLiteral("NV12") : QStringLiteral("BGRA");
				item[QStringLiteral("stage")] = result.strStage;
				item[QStringLiteral("hresult")] = QStringLiteral("0x%1").arg(static_cast<quint32>(result.hr), 8, 16, QLatin1Char('0'));
				item[QStringLiteral("opened")] = result.bOpened;
				item[QStringLiteral("verifiedBytes")] = result.bVerified;
				cases.append(item);
				if ((result.bOpened && !result.bVerified)
					|| (!bCross && format == DXGI_FORMAT_B8G8R8A8_UNORM && !result.bVerified))
				{
					bValid = false;
				}
			}
		}
	}
	QJsonObject result;
	result[QStringLiteral("validProbe")] = bValid;
	result[QStringLiteral("desktopCaptured")] = false;
	result[QStringLiteral("scope")] = QStringLiteral("D3D11 NT-handle/keyed-mutex render targets; not FFmpeg interoperability or D3D12 cross-adapter copies");
	result[QStringLiteral("cases")] = cases;
	std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
	return bValid ? 0 : 1;
}
