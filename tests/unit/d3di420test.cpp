#include "render/d3di420pipeline.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <libyuv.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace
{
using Clock = std::chrono::steady_clock;
double ElapsedUs(Clock::time_point start)
{
	return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

KDecodedVideoFrame MakeFrame(int nWidth, int nHeight, int nPattern)
{
	KDecodedVideoFrame frame;
	frame.nWidth = nWidth;
	frame.nHeight = nHeight;
	frame.pixelFormat = KDecodedPixelFormat::I420;
	auto owner = std::make_shared<std::array<std::vector<unsigned char>, 3>>();
	for (int nPlane = 0; nPlane < 3; ++nPlane)
	{
		const int nPlaneWidth = nPlane ? (nWidth + 1) / 2 : nWidth;
		const int nPlaneHeight = nPlane ? (nHeight + 1) / 2 : nHeight;
		const int nStride = nPlaneWidth + 13;
		auto &bytes = (*owner)[nPlane];
		bytes.resize(static_cast<size_t>(nStride) * nPlaneHeight, 255);
		unsigned int nRandom = 12345 + nPlane;
		for (int nRow = 0; nRow < nPlaneHeight; ++nRow)
			for (int nCol = 0; nCol < nPlaneWidth; ++nCol)
			{
				nRandom = nRandom * 1664525u + 1013904223u;
				int value = nPlane ? 128 : 16 + (219 * nCol / std::max(1, nPlaneWidth - 1));
				if (nPattern == 1)
					value = nPlane ? ((nCol / 4) % 2 ? 240 : 16) : ((nRow / 4) % 2 ? 235 : 16);
				if (nPattern == 2)
					value = 16 + (nRandom % (nPlane ? 225 : 220));
				bytes[static_cast<size_t>(nRow) * nStride + nCol] = static_cast<unsigned char>(value);
			}
		frame.planes[nPlane] = {bytes.data(), nPlaneWidth, nPlaneHeight, nStride, bytes.size()};
	}
	frame.spPlaneOwner = owner;
	return frame;
}

struct KOffscreen
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11VertexShader> vertex;
	ComPtr<ID3D11PixelShader> bgraShader;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11Texture2D> target, staging, bgra;
	ComPtr<ID3D11ShaderResourceView> bgraView;
	ComPtr<ID3D11RenderTargetView> targetView;
	KD3dI420Pipeline pipeline;
	int width = 0, height = 0;

	bool initialize(int w, int h)
	{
		width = w;
		height = h;
		D3D_FEATURE_LEVEL level;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
									 &device, &level, &context)))
			return false;
		const char *vs = "struct O{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};O main(uint id:SV_VertexID){O "
						 "o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}";
		const char *ps = "Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_POSITION,float2 "
						 "uv:TEXCOORD0):SV_TARGET{return t.Sample(s,uv);}";
		ComPtr<ID3DBlob> blob;
		if (FAILED(D3DCompile(vs, std::strlen(vs), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &blob, nullptr)))
			return false;
		if (FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vertex)))
			return false;
		blob.Reset();
		if (FAILED(D3DCompile(ps, std::strlen(ps), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &blob, nullptr)))
			return false;
		if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bgraShader)))
			return false;
		D3D11_SAMPLER_DESC sample = {};
		sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sample.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sample, &sampler)))
			return false;
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)))
			return false;
		if (FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView)))
			return false;
		desc.BindFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
	}
	bool uploadBgra(const KDecodedVideoFrame &frame, const std::vector<unsigned char> &pixels)
	{
		D3D11_TEXTURE2D_DESC desc = {};
		if (bgra)
			bgra->GetDesc(&desc);
		if (!bgra || desc.Width != static_cast<UINT>(frame.nWidth) || desc.Height != static_cast<UINT>(frame.nHeight))
		{
			bgraView.Reset();
			bgra.Reset();
			desc = {};
			desc.Width = frame.nWidth;
			desc.Height = frame.nHeight;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &bgra)))
				return false;
			if (FAILED(device->CreateShaderResourceView(bgra.Get(), nullptr, &bgraView)))
				return false;
		}
		context->UpdateSubresource(bgra.Get(), 0, nullptr, pixels.data(), frame.nWidth * 4, 0);
		return true;
	}
	void draw(bool bI420)
	{
		D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
		context->RSSetViewports(1, &viewport);
		context->OMSetRenderTargets(1, targetView.GetAddressOf(), nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertex.Get(), nullptr, 0);
		if (bI420)
			pipeline.bind(context.Get());
		else
		{
			context->PSSetShader(bgraShader.Get(), nullptr, 0);
			context->PSSetShaderResources(0, 1, bgraView.GetAddressOf());
		}
		context->PSSetSamplers(0, 1, sampler.GetAddressOf());
		context->Draw(3, 0);
		ID3D11ShaderResourceView *empty[3] = {};
		context->PSSetShaderResources(0, 3, empty);
	}
	std::vector<unsigned char> read()
	{
		context->CopyResource(staging.Get(), target.Get());
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return {};
		std::vector<unsigned char> result(static_cast<size_t>(width) * height * 4);
		for (int nRow = 0; nRow < height; ++nRow)
			std::memcpy(result.data() + static_cast<size_t>(nRow) * width * 4,
						static_cast<unsigned char *>(mapped.pData) + static_cast<size_t>(nRow) * mapped.RowPitch,
						width * 4);
		context->Unmap(staging.Get(), 0);
		return result;
	}
};

void Convert(const KDecodedVideoFrame &frame, std::vector<unsigned char> *pPixels)
{
	pPixels->resize(static_cast<size_t>(frame.nWidth) * frame.nHeight * 4);
	libyuv::I420ToARGB(frame.planes[0].pData, frame.planes[0].nStride, frame.planes[1].pData, frame.planes[1].nStride,
					   frame.planes[2].pData, frame.planes[2].nStride, pPixels->data(), frame.nWidth * 4, frame.nWidth,
					   frame.nHeight);
}

double Percentile(std::vector<double> values, double fraction)
{
	if (values.empty())
		return -1;
	std::sort(values.begin(), values.end());
	return values[static_cast<size_t>((values.size() - 1) * fraction)];
}

quint64 CpuTicks()
{
	FILETIME creation, exit, kernel, user;
	GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
	return (static_cast<quint64>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
		   (static_cast<quint64>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
}

QJsonObject Resources(ID3D11Device *pDevice)
{
	PROCESS_MEMORY_COUNTERS_EX memory = {};
	memory.cb = sizeof(memory);
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
	DWORD handles = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handles);
	int threads = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot != INVALID_HANDLE_VALUE)
	{
		THREADENTRY32 entry = {};
		entry.dwSize = sizeof(entry);
		if (Thread32First(snapshot, &entry))
			do
			{
				if (entry.th32OwnerProcessID == GetCurrentProcessId())
					++threads;
			} while (Thread32Next(snapshot, &entry));
		CloseHandle(snapshot);
	}
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIAdapter3> adapter3;
	DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
	bool gpuAvailable = false;
	if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
		SUCCEEDED(adapter.As(&adapter3)))
		gpuAvailable = SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
					   SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared));
	return {{"gpuAvailable", gpuAvailable},
			{"gpuLocalBytes", double(local.CurrentUsage)},
			{"gpuNonLocalBytes", double(shared.CurrentUsage)},
			{"privateBytes", static_cast<double>(memory.PrivateUsage)},
			{"workingSetBytes", static_cast<double>(memory.WorkingSetSize)},
			{"handles", static_cast<int>(handles)},
			{"threads", threads}};
}

int Benchmark(const QStringList &arguments)
{
	if (arguments.size() != 8)
		return 10;
	const int width = arguments[2].toInt(), height = arguments[3].toInt(), fps = arguments[4].toInt();
	const bool i420 = arguments[5] == QStringLiteral("i420");
	const int seconds = arguments[6].toInt(), warmup = arguments[7].toInt();
	if (width <= 0 || height <= 0 || fps <= 0 || seconds <= 0 || warmup < 0 ||
		(arguments[5] != QStringLiteral("i420") && arguments[5] != QStringLiteral("bgra")))
		return 10;
	KOffscreen gpu;
	if (!gpu.initialize(width, height))
		return 11;
	auto frame = MakeFrame(width, height, 2);
	struct KQuery
	{
		ComPtr<ID3D11Query> disjoint, start, end;
		bool pending = false, measured = false;
	};
	std::array<KQuery, 4> queries;
	for (auto &query : queries)
	{
		D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		if (FAILED(gpu.device->CreateQuery(&desc, &query.disjoint)))
			return 12;
		desc.Query = D3D11_QUERY_TIMESTAMP;
		if (FAILED(gpu.device->CreateQuery(&desc, &query.start)) || FAILED(gpu.device->CreateQuery(&desc, &query.end)))
			return 12;
	}
	std::vector<double> prepare, upload, draw, gpuTimes;
	int missed = 0, gpuMissed = 0;
	QJsonObject first;
	quint64 cpuStart = 0;
	const int warmupFrames = warmup * fps, total = (warmup + seconds) * fps;
	const auto start = Clock::now();
	for (int index = 0; index < total; ++index)
	{
		const auto deadline = start + std::chrono::microseconds(static_cast<long long>(index) * 1000000 / fps);
		std::this_thread::sleep_until(deadline);
		const bool measured = index >= warmupFrames;
		if (index == warmupFrames)
		{
			first = Resources(gpu.device.Get());
			cpuStart = CpuTicks();
		}
		if (measured && Clock::now() > deadline + std::chrono::microseconds(1000000 / fps))
			++missed;
		auto &query = queries[index % queries.size()];
		if (query.pending)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
			UINT64 begin = 0, end = 0;
			const bool ready =
				gpu.context->GetData(query.disjoint.Get(), &disjoint, sizeof(disjoint),
									 D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				gpu.context->GetData(query.start.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
					S_OK &&
				gpu.context->GetData(query.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
			if (query.measured)
			{
				if (ready && !disjoint.Disjoint && disjoint.Frequency)
					gpuTimes.push_back(double(end - begin) * 1e6 / disjoint.Frequency);
				else
					++gpuMissed;
			}
		}
		const auto prepareStart = Clock::now();
		std::vector<unsigned char> pixels;
		if (!i420)
			Convert(frame, &pixels);
		const double prepareUs = ElapsedUs(prepareStart);
		gpu.context->Begin(query.disjoint.Get());
		gpu.context->End(query.start.Get());
		const auto uploadStart = Clock::now();
		if (i420)
		{
			if (FAILED(gpu.pipeline.upload(gpu.device.Get(), gpu.context.Get(), frame)))
				return 13;
		}
		else if (!gpu.uploadBgra(frame, pixels))
			return 14;
		const double uploadUs = ElapsedUs(uploadStart);
		const auto drawStart = Clock::now();
		gpu.draw(i420);
		const double drawUs = ElapsedUs(drawStart);
		gpu.context->End(query.end.Get());
		gpu.context->End(query.disjoint.Get());
		gpu.context->Flush();
		query.pending = true;
		query.measured = measured;
		if (measured)
		{
			prepare.push_back(prepareUs);
			upload.push_back(uploadUs);
			draw.push_back(drawUs);
		}
	}
	// Last four queries are intentionally excluded rather than blocking for GPU completion.
	QJsonObject result{{"mode", arguments[5]},
					   {"width", width},
					   {"height", height},
					   {"fps", fps},
					   {"warmupSeconds", warmup},
					   {"sampleSeconds", seconds},
					   {"frames", int(prepare.size())},
					   {"cpuSeconds", double(CpuTicks() - cpuStart) / 1e7},
					   {"missedDeadlines", missed},
					   {"gpuSamples", int(gpuTimes.size())},
					   {"gpuUnavailableSamples", gpuMissed},
					   {"gpuTailExcluded", 4},
					   {"before", first},
					   {"after", Resources(gpu.device.Get())},
					   {"uploadBytesPerFrame", double(width) * height * (i420 ? 1.5 : 4.0)},
					   {"prepareP50Us", Percentile(prepare, .5)},
					   {"prepareP95Us", Percentile(prepare, .95)},
					   {"uploadSubmitP50Us", Percentile(upload, .5)},
					   {"uploadSubmitP95Us", Percentile(upload, .95)},
					   {"drawCallP50Us", Percentile(draw, .5)},
					   {"drawCallP95Us", Percentile(draw, .95)},
					   {"gpuUploadDrawP50Us", Percentile(gpuTimes, .5)},
					   {"gpuUploadDrawP95Us", Percentile(gpuTimes, .95)}};
	std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
	return 0;
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (app.arguments().contains(QStringLiteral("--benchmark")))
		return Benchmark(app.arguments());
	KOffscreen gpu;
	if (!gpu.initialize(97, 65))
		return 1;
	int maximumError = 0;
	for (int pattern = 0; pattern < 3; ++pattern)
		for (const auto size : {std::pair<int, int>{97, 65}, {48, 32}, {194, 130}})
		{
			auto frame = MakeFrame(size.first, size.second, pattern);
			auto invalid = frame;
			invalid.planes[1].nStride = 0;
			if (invalid.hasPixels() || !frame.hasPixels())
				return 2;
			invalid = frame;
			invalid.planes[0].nBytes = 1;
			if (invalid.hasPixels())
				return 2;
			std::vector<unsigned char> cpu;
			Convert(frame, &cpu);
			if (!gpu.uploadBgra(frame, cpu))
				return 3;
			gpu.draw(false);
			auto expected = gpu.read();
			if (FAILED(gpu.pipeline.upload(gpu.device.Get(), gpu.context.Get(), frame)))
				return 4;
			gpu.draw(true);
			auto actual = gpu.read();
			if (expected.empty() || expected.size() != actual.size())
				return 5;
			for (size_t index = 0; index < actual.size(); ++index)
				maximumError = std::max(maximumError, std::abs(int(expected[index]) - int(actual[index])));
			gpu.pipeline.clear();
			if (gpu.pipeline.isReady())
				return 6;
		}
	// Force a genuine texture-creation failure without allocating a large CPU image.
	// D3D11 supports at most 16384 texels per dimension (lower on feature level 10).
	auto oversized = MakeFrame(32769, 1, 0);
	if (SUCCEEDED(gpu.pipeline.upload(gpu.device.Get(), gpu.context.Get(), oversized)) || gpu.pipeline.isReady())
		return 8;
	for (int nCycle = 0; nCycle < 12; ++nCycle)
	{
		auto frame = MakeFrame(nCycle % 2 ? 97 : 48, nCycle % 2 ? 65 : 32, 2);
		std::weak_ptr<const void> weakOwner = frame.spPlaneOwner;
		if (FAILED(gpu.pipeline.upload(gpu.device.Get(), gpu.context.Get(), frame)))
			return 9;
		gpu.draw(true);
		// UpdateSubresource must not leave the CPU frame retained by the pipeline.
		frame = {};
		if (!weakOwner.expired())
			return 9;
	}
	gpu.pipeline.clear();
	if (gpu.pipeline.isReady())
		return 9;
	std::cout << "D3D I420 grayscale/color/random, odd sizes, padded strides, 1x/up/downscale maxError=" << maximumError
			  << '\n';
	return maximumError <= 3 ? 0 : 7;
}
