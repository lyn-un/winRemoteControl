#include "capture/hdrgpubgraconverter.h"
#include "capture/hdrtonemapper.h"
#include "capture/mappedframeconversion.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace
{
double Microseconds(Clock::time_point start)
{
	return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double CpuMilliseconds()
{
	FILETIME created, exited, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
		return -1;
	ULARGE_INTEGER kernelTicks = {}, userTicks = {};
	kernelTicks.LowPart = kernel.dwLowDateTime;
	kernelTicks.HighPart = kernel.dwHighDateTime;
	userTicks.LowPart = user.dwLowDateTime;
	userTicks.HighPart = user.dwHighDateTime;
	return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) / 10000.0;
}

struct KMeasurements
{
	std::vector<double> total, map, copy, submit, gpuConvert, gpuCopy;
	void write(QJsonObject &result, const char *pName, std::vector<double> values)
	{
		if (values.empty())
			return;
		std::sort(values.begin(), values.end());
		QJsonObject metrics;
		for (const int nPercent : {50, 95, 99})
			metrics[QStringLiteral("p%1Us").arg(nPercent)] = values[(values.size() - 1) * nPercent / 100];
		result[QString::fromLatin1(pName)] = metrics;
	}
};

struct KQueries
{
	ComPtr<ID3D11Query> disjoint, start, converted, copied;
	bool bPending = false;
	bool bSample = false;
};
}

int main(int nArgc, char **pArgv)
{
	QCoreApplication app(nArgc, pArgv);
	const auto args = app.arguments();
	if (args.size() != 6)
	{
		std::cerr << "Usage: benchmark cpu|gpu|desktop|d2d width height fps repetition\n";
		return 2;
	}
	const bool bToneMap = args[1] == QStringLiteral("d2d");
	const bool bDesktop = args[1] == QStringLiteral("desktop");
	const bool bGpu = args[1] == QStringLiteral("gpu") || bToneMap || bDesktop;
	const int nWidth = args[2].toInt(), nHeight = args[3].toInt(), nFps = args[4].toInt();
	if ((!bGpu && args[1] != QStringLiteral("cpu")) || nWidth <= 0 || nHeight <= 0
		|| nWidth > 4096 || nHeight > 4096 || nFps <= 0 || nFps > 144)
		return 2;
	ComPtr<ID3D11Device> spDevice;
	ComPtr<ID3D11DeviceContext> spContext;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION, &spDevice, nullptr, &spContext);
	if (FAILED(hr))
		return 77;
	ComPtr<IDXGIDevice> spDxgiDevice;
	ComPtr<IDXGIAdapter> spAdapter;
	spDevice.As(&spDxgiDevice);
	spDxgiDevice->GetAdapter(&spAdapter);
	DXGI_ADAPTER_DESC adapterDesc = {};
	spAdapter->GetDesc(&adapterDesc);
	LARGE_INTEGER driverVersion = {};
	spAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion);
	std::vector<std::uint16_t> pixels(static_cast<size_t>(nWidth) * nHeight * 4);
	unsigned int nRandom = 12345;
	for (auto &pixel : pixels)
	{
		nRandom = nRandom * 1664525u + 1013904223u;
		pixel = static_cast<std::uint16_t>(0x1800 + nRandom % 0x3000);
	}
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = nWidth;
	desc.Height = nHeight;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA initial = {};
	initial.pSysMem = pixels.data();
	initial.SysMemPitch = nWidth * 8;
	ComPtr<ID3D11Texture2D> spInput, spStaging;
	if (FAILED(spDevice->CreateTexture2D(&desc, &initial, &spInput)))
		return 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (bGpu)
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	if (FAILED(spDevice->CreateTexture2D(&desc, nullptr, &spStaging)))
		return 1;
	std::array<KQueries, 8> queries;
	for (auto &query : queries)
	{
		D3D11_QUERY_DESC queryDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		if (FAILED(spDevice->CreateQuery(&queryDesc, &query.disjoint)))
			return 1;
		queryDesc.Query = D3D11_QUERY_TIMESTAMP;
		if (FAILED(spDevice->CreateQuery(&queryDesc, &query.start))
			|| FAILED(spDevice->CreateQuery(&queryDesc, &query.converted))
			|| FAILED(spDevice->CreateQuery(&queryDesc, &query.copied)))
			return 1;
	}
	KHdrGpuBgraConverter converter;
	converter.setDesktopToneMapping(bDesktop);
	KHdrToneMapper toneMapper;
	KCaptureFrame frame;
	KMeasurements measurements;
	std::uint64_t nFrame = 0, nMissed = 0, nInvalidQueries = 0;
	UINT nRowPitch = 0;
	const auto start = Clock::now();
	const auto sampleStart = start + std::chrono::seconds(10);
	const auto end = sampleStart + std::chrono::seconds(60);
	const auto interval = std::chrono::microseconds(1000000 / nFps);
	auto deadline = start;
	double fCpuStart = -1, fCpuEnd = -1;
	DWORD nHandlesStart = 0;
	while (Clock::now() < end)
	{
		const bool bSample = Clock::now() >= sampleStart;
		if (bSample && fCpuStart < 0)
		{
			fCpuStart = CpuMilliseconds();
			GetProcessHandleCount(GetCurrentProcess(), &nHandlesStart);
		}
		auto &query = queries[nFrame % queries.size()];
		if (query.bPending)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
			UINT64 nStart = 0, nConverted = 0, nCopied = 0;
			if (spContext->GetData(query.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
				&& !disjoint.Disjoint && disjoint.Frequency
				&& spContext->GetData(query.start.Get(), &nStart, sizeof(nStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
				&& spContext->GetData(query.converted.Get(), &nConverted, sizeof(nConverted), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK
				&& spContext->GetData(query.copied.Get(), &nCopied, sizeof(nCopied), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
			{
				if (query.bSample)
				{
					measurements.gpuConvert.push_back((nConverted - nStart) * 1e6 / disjoint.Frequency);
					measurements.gpuCopy.push_back((nCopied - nConverted) * 1e6 / disjoint.Frequency);
				}
			}
			else if (query.bSample)
				++nInvalidQueries;
		}
		query.bPending = true;
		query.bSample = bSample;
		const auto frameStart = Clock::now();
		spContext->Begin(query.disjoint.Get());
		spContext->End(query.start.Get());
		ComPtr<ID3D11Texture2D> spConverted;
		ComPtr<ID3D11Texture2D> spToneMapped;
		if (bToneMap && FAILED(toneMapper.convert(spDevice.Get(), spContext.Get(),
			spInput.Get(), 2.5f, 1000.0f, &spToneMapped)))
			return 1;
		if (bGpu && FAILED(converter.convert(spDevice.Get(), spContext.Get(),
			bToneMap ? spToneMapped.Get() : spInput.Get(), 2.5f, &spConverted)))
			return 1;
		spContext->End(query.converted.Get());
		spContext->CopyResource(spStaging.Get(), bGpu ? spConverted.Get() : spInput.Get());
		spContext->End(query.copied.Get());
		spContext->End(query.disjoint.Get());
		const double fSubmit = Microseconds(frameStart);
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		const auto mapStart = Clock::now();
		if (FAILED(spContext->Map(spStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return 1;
		const double fMap = Microseconds(mapStart);
		nRowPitch = mapped.RowPitch;
		const auto copyStart = Clock::now();
		if (!CopyMappedCaptureFrameToBgra(desc, mapped, &frame, 2.5f))
			return 1;
		const double fCopy = Microseconds(copyStart);
		spContext->Unmap(spStaging.Get(), 0);
		const double fTotal = Microseconds(frameStart);
		if (bSample)
		{
			measurements.total.push_back(fTotal);
			measurements.submit.push_back(fSubmit);
			measurements.map.push_back(fMap);
			measurements.copy.push_back(fCopy);
			if (Clock::now() > deadline + interval)
				++nMissed;
		}
		++nFrame;
		deadline += interval;
		if (deadline < Clock::now())
			deadline = Clock::now();
		std::this_thread::sleep_until(deadline);
	}
	fCpuEnd = CpuMilliseconds();
	PROCESS_MEMORY_COUNTERS memory = {};
	GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
	DWORD nHandlesEnd = 0;
	GetProcessHandleCount(GetCurrentProcess(), &nHandlesEnd);
	QJsonObject result;
	result["mode"] = args[1];
	result["width"] = nWidth;
	result["height"] = nHeight;
	result["fps"] = nFps;
	result["repetition"] = args[5].toInt();
	result["adapter"] = QString::fromWCharArray(adapterDesc.Description);
	result["driverVersionRaw"] = QString::number(driverVersion.QuadPart);
	result["sampleFrames"] = static_cast<qint64>(measurements.total.size());
	result["missedDeadlines"] = static_cast<qint64>(nMissed);
	result["cpuMs"] = fCpuEnd - fCpuStart;
	result["effectiveBytesPerFrame"] = static_cast<qint64>(nWidth) * nHeight * (bGpu ? 4 : 8);
	result["rowPitch"] = static_cast<qint64>(nRowPitch);
	result["workingSetBytes"] = static_cast<qint64>(memory.WorkingSetSize);
	result["handlesAtSampleStart"] = static_cast<qint64>(nHandlesStart);
	result["handlesAtSampleEnd"] = static_cast<qint64>(nHandlesEnd);
	result["invalidQueries"] = static_cast<qint64>(nInvalidQueries);
	result["queryTailExcluded"] = 8;
	result["warmupSeconds"] = 10;
	result["sampleSeconds"] = 60;
	if (bToneMap)
	{
		result["referencePeakNits"] = 1000;
		result["referencePeakSource"] = "fixed_evaluation_policy_not_content_measurement";
	}
	measurements.write(result, "total", measurements.total);
	measurements.write(result, "cpuSubmit", measurements.submit);
	measurements.write(result, "mapWait", measurements.map);
	measurements.write(result, "cpuConvertOrCopy", measurements.copy);
	measurements.write(result, "gpuConvertIncludingInputCopy", measurements.gpuConvert);
	measurements.write(result, "gpuReadbackCopy", measurements.gpuCopy);
	std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
	return 0;
}
