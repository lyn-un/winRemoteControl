// Synthetic GPU source only. No desktop duplication, input injection or network.
#include "capture/d3d11bgraframe.h"
#include "capture/gpunv12converter.h"
#include "codec/h264encoder.h"
#include "common/resourcetracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using KClock = std::chrono::steady_clock;

namespace
{
constexpr char kSourceShader[] = R"(
cbuffer Parameters : register(b0) { uint frame; uint3 padding; };
float4 VS(uint id : SV_VertexID) : SV_Position
{
 float2 uv = float2((id << 1) & 2, id & 2);
 return float4(uv * float2(2,-2) + float2(-1,1),0,1);
}
uint Hash(uint value)
{
 value ^= value >> 16; value *= 0x7feb352d;
 value ^= value >> 15; value *= 0x846ca68b;
 return value ^ (value >> 16);
}
float4 PS(float4 position : SV_Position) : SV_Target
{
 uint value = Hash(uint(position.x)/4 + uint(position.y)/4*4099 + frame*17 + 0x9e3779b9);
 return float4(float3(value & 255, (value >> 8) & 255, (value >> 16) & 255)/255.0,1);
}
)";

double CpuMs(bool bThread)
{
	FILETIME created = {}, exited = {}, kernel = {}, user = {};
	const bool bOk = bThread ? ::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, &user)
		: ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
	if (!bOk)
		return -1;
	ULARGE_INTEGER kernelTicks = {}, userTicks = {};
	kernelTicks.LowPart = kernel.dwLowDateTime;
	kernelTicks.HighPart = kernel.dwHighDateTime;
	userTicks.LowPart = user.dwLowDateTime;
	userTicks.HighPart = user.dwHighDateTime;
	return static_cast<double>(kernelTicks.QuadPart + userTicks.QuadPart) / 10000.0;
}

QJsonObject Percentiles(std::vector<double> values)
{
	QJsonObject result;
	if (values.empty())
		return result;
	std::sort(values.begin(), values.end());
	for (int nPercent : {50, 95, 99})
		result[QStringLiteral("p%1Us").arg(nPercent)] = values[(values.size() - 1) * nPercent / 100];
	result[QStringLiteral("samples")] = static_cast<qint64>(values.size());
	return result;
}

// Low-frequency resource sampling is kept off the frame producer. Peaks below
// are observed one-second sample maxima, not continuous hardware high-water marks.
class KResourceSampler
{
public:
	~KResourceSampler()
	{
		stop();
	}
	void start(KClock::time_point sampleStart)
	{
		m_thread = std::thread([this, sampleStart]()
		{
			KResourceTraceLogger::snapshot(); // Initialize diagnostics during warmup.
			std::unique_lock<std::mutex> lock(m_mutex);
			auto due = sampleStart;
			while (!m_condition.wait_until(lock, due, [this]() { return m_bStopped; }))
			{
				lock.unlock();
				const auto snapshot = KResourceTraceLogger::snapshot();
				const double fElapsed = std::chrono::duration<double>(KClock::now() - sampleStart).count();
				QJsonObject sample;
				sample[QStringLiteral("elapsedSeconds")] = fElapsed;
				sample[QStringLiteral("privateBytes")] = snapshot.bMemoryAvailable
					? QJsonValue(QString::number(snapshot.nPrivateBytes)) : QJsonValue(QJsonValue::Null);
				sample[QStringLiteral("gpuDedicatedBytes")] = snapshot.bGpuAvailable
					? QJsonValue(QString::number(snapshot.nGpuDedicatedBytes)) : QJsonValue(QJsonValue::Null);
				sample[QStringLiteral("gpuSharedBytes")] = snapshot.bGpuAvailable
					? QJsonValue(QString::number(snapshot.nGpuSharedBytes)) : QJsonValue(QJsonValue::Null);
				sample[QStringLiteral("handles")] = snapshot.bHandleCountAvailable
					? QJsonValue(static_cast<int>(snapshot.nHandleCount)) : QJsonValue(QJsonValue::Null);
				sample[QStringLiteral("threads")] = snapshot.bThreadCountAvailable
					? QJsonValue(static_cast<int>(snapshot.nThreadCount)) : QJsonValue(QJsonValue::Null);
				m_samples.append(sample);
				lock.lock();
				due = KClock::now() + std::chrono::seconds(1);
			}
		});
	}
	void stop()
	{
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			m_bStopped = true;
		}
		m_condition.notify_one();
		if (m_thread.joinable())
			m_thread.join();
	}
	QJsonObject result() const
	{
		QJsonObject result;
		result[QStringLiteral("nominalIntervalSeconds")] = 1;
		result[QStringLiteral("samples")] = m_samples;
		for (const char *pName : {"privateBytes", "gpuDedicatedBytes", "gpuSharedBytes", "handles", "threads"})
		{
			quint64 nMaximum = 0;
			bool bAvailable = false;
			const QString name = QString::fromLatin1(pName);
			for (const auto &sample : m_samples)
			{
				const auto value = sample.toObject().value(name);
				if (value.isNull())
					continue;
				const quint64 nValue = value.isString() ? value.toString().toULongLong()
					: static_cast<quint64>(value.toInteger());
				nMaximum = std::max(nMaximum, nValue);
				bAvailable = true;
			}
			result[name + QStringLiteral("ObservedMax")] = bAvailable
				? QJsonValue(QString::number(nMaximum)) : QJsonValue(QJsonValue::Null);
		}
		return result;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	std::thread m_thread;
	bool m_bStopped = false;
	QJsonArray m_samples;
};

class KValidator
{
public:
	KValidator(int nWidth, int nHeight)
		: m_nWidth(nWidth)
		, m_nHeight(nHeight)
	{
	}
	~KValidator()
	{
		finish();
	}
	bool start()
	{
		m_pCodec = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
		if (m_pCodec == nullptr)
			return false;
		m_pCodec->thread_count = 4;
		m_pCodec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		if (avcodec_open2(m_pCodec, m_pCodec->codec, nullptr) < 0)
		{
			avcodec_free_context(&m_pCodec);
			return false;
		}
		m_thread = std::thread([this]() { run(); });
		return true;
	}
	void enqueue(const QByteArray &data, qint64 nPts)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		nMaxPacketBytes = (std::max)(nMaxPacketBytes, static_cast<quint64>(data.size()));
		if (m_nQueuedBytes + data.size() > 4 * 1024 * 1024 || m_packets.size() >= 128)
		{
			fail(QueueOverflowFailure);
			return;
		}
		m_packets.push_back({data, nPts});
		m_nQueuedBytes += data.size();
		nPeakQueuedBytes = (std::max)(nPeakQueuedBytes, static_cast<quint64>(m_nQueuedBytes));
		m_condition.notify_one();
	}
	void finish()
	{
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			m_bFinished = true;
		}
		m_condition.notify_one();
		if (m_thread.joinable())
			m_thread.join();
		avcodec_free_context(&m_pCodec);
	}
	std::atomic_bool valid = true;
	int nDecoded = 0;
	quint64 nMaxPacketBytes = 0;
	quint64 nPeakQueuedBytes = 0;
	QString error() const
	{
		switch (m_failure.load())
		{
		case QueueOverflowFailure: return QStringLiteral("validator_queue_overflow");
		case AllocationFailure: return QStringLiteral("validator_allocation_failed");
		case DimensionFailure: return QStringLiteral("validator_dimensions_changed");
		case DuplicatePtsFailure: return QStringLiteral("validator_duplicate_pts");
		case ReceiveFailure: return QStringLiteral("validator_receive_failed");
		case SendFailure: return QStringLiteral("validator_send_failed");
		case FlushFailure: return QStringLiteral("validator_flush_failed");
		default: return {};
		}
	}

private:
	enum Failure
	{
		NoFailure, QueueOverflowFailure, AllocationFailure, DimensionFailure,
		DuplicatePtsFailure, ReceiveFailure, SendFailure, FlushFailure
	};
	void fail(Failure failure)
	{
		int nExpected = NoFailure;
		m_failure.compare_exchange_strong(nExpected, failure);
		valid = false;
	}
	struct KPacket
	{
		QByteArray data;
		qint64 nPts = 0;
	};
	bool drain(AVFrame *pFrame)
	{
		int nResult = 0;
		while ((nResult = avcodec_receive_frame(m_pCodec, pFrame)) >= 0)
		{
			if (pFrame->width != m_nWidth || pFrame->height != m_nHeight)
			{
				fail(DimensionFailure);
				return false;
			}
			if (!m_seen.insert(pFrame->pts).second)
			{
				fail(DuplicatePtsFailure);
				return false;
			}
			++nDecoded;
			av_frame_unref(pFrame);
		}
		if (nResult != AVERROR(EAGAIN) && nResult != AVERROR_EOF)
		{
			fail(ReceiveFailure);
			return false;
		}
		return true;
	}
	void run()
	{
		AVPacket *pPacket = av_packet_alloc();
		AVFrame *pFrame = av_frame_alloc();
		if (pPacket == nullptr || pFrame == nullptr)
			fail(AllocationFailure);
		while (valid)
		{
			KPacket next;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_condition.wait(lock, [this]() { return m_bFinished || !m_packets.empty(); });
				if (m_packets.empty() && m_bFinished)
					break;
				next = std::move(m_packets.front());
				m_packets.pop_front();
				m_nQueuedBytes -= next.data.size();
			}
			if (av_new_packet(pPacket, static_cast<int>(next.data.size())) < 0)
			{
				fail(AllocationFailure);
				break;
			}
			std::memcpy(pPacket->data, next.data.constData(), next.data.size());
			pPacket->pts = pPacket->dts = next.nPts;
			int nResult = avcodec_send_packet(m_pCodec, pPacket);
			if (nResult == AVERROR(EAGAIN) && drain(pFrame))
				nResult = avcodec_send_packet(m_pCodec, pPacket);
			if (nResult < 0)
				fail(SendFailure);
			else
				drain(pFrame);
			av_packet_unref(pPacket);
		}
		if (valid && (avcodec_send_packet(m_pCodec, nullptr) < 0 || !drain(pFrame)))
			fail(FlushFailure);
		av_packet_free(&pPacket);
		av_frame_free(&pFrame);
	}
	int m_nWidth = 0;
	int m_nHeight = 0;
	AVCodecContext *m_pCodec = nullptr;
	std::mutex m_mutex;
	std::condition_variable m_condition;
	std::deque<KPacket> m_packets;
	qsizetype m_nQueuedBytes = 0;
	bool m_bFinished = false;
	std::thread m_thread;
	std::set<qint64> m_seen;
	std::atomic_int m_failure = NoFailure;
};

struct KSource
{
	std::shared_ptr<std::recursive_mutex> spLock = std::make_shared<std::recursive_mutex>();
	bool bAdapterUnavailable = false;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> texture, staging;
	ComPtr<ID3D11RenderTargetView> target;
	ComPtr<ID3D11VertexShader> vertex;
	ComPtr<ID3D11PixelShader> pixel;
	ComPtr<ID3D11Buffer> parameters;
	DXGI_ADAPTER_DESC1 adapter = {};
	quint64 nLuid = 0;
	LARGE_INTEGER driverVersion = {};
	bool initialize(quint32 nVendor, int nWidth, int nHeight, bool bCpu)
	{
		ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			return false;
		ComPtr<IDXGIAdapter1> selected;
		for (UINT nIndex = 0; ; ++nIndex)
		{
			ComPtr<IDXGIAdapter1> candidate;
			const HRESULT hr = factory->EnumAdapters1(nIndex, &candidate);
			if (hr == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(hr))
				return false;
			if (SUCCEEDED(candidate->GetDesc1(&adapter)) && adapter.VendorId == nVendor
				&& !(adapter.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
			{
				selected = candidate;
				break;
			}
		}
		if (!selected)
		{
			bAdapterUnavailable = true;
			return false;
		}
		if (FAILED(D3D11CreateDevice(selected.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
			D3D11_SDK_VERSION, &device, nullptr, &context)))
		{
			return false;
		}
		nLuid = (static_cast<quint64>(static_cast<quint32>(adapter.AdapterLuid.HighPart)) << 32)
			| adapter.AdapterLuid.LowPart;
		selected->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion);
		D3D11_TEXTURE2D_DESC descriptor = {};
		descriptor.Width = nWidth;
		descriptor.Height = nHeight;
		descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
		descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (FAILED(device->CreateTexture2D(&descriptor, nullptr, &texture))
			|| FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)))
		{
			return false;
		}
		if (bCpu)
		{
			descriptor.Usage = D3D11_USAGE_STAGING;
			descriptor.BindFlags = 0;
			descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(device->CreateTexture2D(&descriptor, nullptr, &staging)))
				return false;
		}
		ComPtr<ID3DBlob> vs, ps, error;
		if (FAILED(D3DCompile(kSourceShader, sizeof(kSourceShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &error))
			|| FAILED(D3DCompile(kSourceShader, sizeof(kSourceShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &error))
			|| FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex))
			|| FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel)))
		{
			return false;
		}
		D3D11_BUFFER_DESC buffer = {};
		buffer.ByteWidth = 16;
		buffer.Usage = D3D11_USAGE_DEFAULT;
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		return SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &parameters));
	}
	void draw(quint32 nFrame, int nWidth, int nHeight)
	{
		std::lock_guard<std::recursive_mutex> guard(*spLock);
		const quint32 constants[4] = {nFrame, 0, 0, 0};
		context->UpdateSubresource(parameters.Get(), 0, nullptr, constants, 0, 0);
		ID3D11RenderTargetView *pTarget = target.Get();
		ID3D11Buffer *pParameters = parameters.Get();
		context->OMSetRenderTargets(1, &pTarget, nullptr);
		const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(nWidth), static_cast<float>(nHeight), 0, 1};
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertex.Get(), nullptr, 0);
		context->PSSetShader(pixel.Get(), nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &pParameters);
		context->Draw(3, 0);
		context->OMSetRenderTargets(0, nullptr, nullptr);
	}
};

QJsonObject ProfileConversionGpu(KSource &source, int nWidth, int nHeight)
{
	QJsonObject result;
	result[QStringLiteral("scope")] = QStringLiteral("isolated RGB-to-NV12 including input copy; excludes encoder");
	result[QStringLiteral("available")] = false;
	ComPtr<ID3D11Texture2D> destination;
	D3D11_TEXTURE2D_DESC descriptor = {};
	descriptor.Width = (nWidth + 15) & ~15;
	descriptor.Height = (nHeight + 15) & ~15;
	descriptor.MipLevels = descriptor.ArraySize = descriptor.SampleDesc.Count = 1;
	descriptor.Format = DXGI_FORMAT_NV12;
	descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
	KGpuNv12Converter converter;
	if (FAILED(source.device->CreateTexture2D(&descriptor, nullptr, &destination))
		|| FAILED(converter.convert(source.context.Get(), source.texture.Get(), destination.Get(), nWidth, nHeight)))
	{
		return result;
	}
	struct KQueries
	{
		ComPtr<ID3D11Query> disjoint, start, finish;
		bool bPending = false;
		bool bSample = false;
	};
	KQueries queries[8];
	for (auto &query : queries)
	{
		D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		if (FAILED(source.device->CreateQuery(&queryDesc, &query.disjoint)))
			return result;
		queryDesc.Query = D3D11_QUERY_TIMESTAMP;
		if (FAILED(source.device->CreateQuery(&queryDesc, &query.start))
			|| FAILED(source.device->CreateQuery(&queryDesc, &query.finish)))
		{
			return result;
		}
	}
	std::vector<double> times;
	int nInvalid = 0, nBusy = 0;
	const auto Collect = [&](KQueries &query)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
		UINT64 nStart = 0, nFinish = 0;
		if (source.context->GetData(query.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
			|| source.context->GetData(query.start.Get(), &nStart, sizeof(nStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
			|| source.context->GetData(query.finish.Get(), &nFinish, sizeof(nFinish), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		{
			return false;
		}
		query.bPending = false;
		if (query.bSample)
		{
			if (!disjoint.Disjoint && disjoint.Frequency != 0 && nFinish >= nStart)
				times.push_back((nFinish - nStart) * 1e6 / disjoint.Frequency);
			else
				++nInvalid;
		}
		return true;
	};
	for (int nIndex = 0; nIndex < 512; ++nIndex)
	{
		KQueries &query = queries[nIndex % 8];
		if (query.bPending)
			Collect(query);
		source.draw(nIndex, nWidth, nHeight);
		const bool bMeasure = !query.bPending;
		if (bMeasure)
		{
			query.bPending = true;
			query.bSample = nIndex >= 16;
			source.context->Begin(query.disjoint.Get());
			source.context->End(query.start.Get());
		}
		else
		{
			++nBusy;
		}
		if (FAILED(converter.convert(source.context.Get(), source.texture.Get(), destination.Get(), nWidth, nHeight)))
			return result;
		if (bMeasure)
		{
			source.context->End(query.finish.Get());
			source.context->End(query.disjoint.Get());
		}
		source.context->Flush();
	}
	int nTail = 0;
	for (auto &query : queries)
	{
		if (query.bPending && !Collect(query))
			++nTail;
	}
	result[QStringLiteral("available")] = !times.empty();
	result[QStringLiteral("gpuCommandSpan")] = Percentiles(times);
	result[QStringLiteral("invalidQueries")] = nInvalid;
	result[QStringLiteral("busyQuerySlots")] = nBusy;
	result[QStringLiteral("tailExcludedWithoutWait")] = nTail;
	return result;
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const auto args = app.arguments();
	const bool bSmoke = args.size() == 8 && args[7] == QStringLiteral("--smoke");
	if (args.size() != 7 && !bSmoke)
	{
		std::cerr << "Usage: benchmark cpu|gpu h264_qsv|h264_amf|h264_nvenc width height fps repetition [--smoke]\n";
		return 2;
	}
	const bool bGpu = args[1] == QStringLiteral("gpu");
	const int nWidth = args[3].toInt(), nHeight = args[4].toInt(), nFps = args[5].toInt();
	const int nRepetition = args[6].toInt();
	KVideoEncoderPreference preference = AutoVideoEncoderPreference;
	quint32 nVendor = 0;
	if (args[2] == QStringLiteral("h264_qsv"))
	{
		preference = QuickSyncVideoEncoderPreference;
		nVendor = 0x8086;
	}
	if (args[2] == QStringLiteral("h264_amf"))
	{
		preference = AmfVideoEncoderPreference;
		nVendor = 0x1002;
	}
	if (args[2] == QStringLiteral("h264_nvenc"))
	{
		preference = NvencVideoEncoderPreference;
		nVendor = 0x10de;
	}
	if ((!bGpu && args[1] != QStringLiteral("cpu")) || nVendor == 0 || nRepetition < 1 || nRepetition > 3
		|| (nWidth != 1920 && nWidth != 2560) || nHeight != nWidth * 9 / 16 || (nFps != 30 && nFps != 60))
	{
		return 2;
	}
	KSource source;
	if (!source.initialize(nVendor, nWidth, nHeight, !bGpu))
	{
		std::cerr << (source.bAdapterUnavailable ? "Requested adapter is unavailable" : "GPU source initialization failed")
			<< std::endl;
		return source.bAdapterUnavailable ? 77 : 1;
	}
	KD3D11BgraFrame native(source.texture.Get(), source.spLock, 1, source.nLuid);
	KValidator validator(nWidth, nHeight);
	if (!validator.start())
		return 1;
	KH264Encoder encoder(preference);
	QString strError;
	if (!encoder.setLevelLimit(52) || !encoder.openStream(nWidth, nHeight, nFps, 12000,
		[](const QByteArray &) {}, &strError)
		|| encoder.encoderName() != args[2] || (bGpu && !encoder.enableGpuFrames(native, &strError)))
	{
		std::cerr << strError.toStdString() << std::endl;
		return 1;
	}
	std::vector<unsigned char> bgra, y, u, v;
	SwsContext *pSws = nullptr;
	if (!bGpu)
	{
		bgra.resize(static_cast<size_t>(nWidth) * nHeight * 4);
		y.resize(static_cast<size_t>(nWidth) * nHeight);
		u.resize(static_cast<size_t>(nWidth) * nHeight / 4);
		v.resize(u.size());
		pSws = sws_getContext(nWidth, nHeight, AV_PIX_FMT_BGRA, nWidth, nHeight,
			AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		if (pSws == nullptr)
			return 1;
	}
	struct KPending
	{
		KClock::time_point started;
		bool bSample = false;
	};
	std::map<qint64, KPending> pending;
	std::vector<double> processingUs, outputUs;
	quint64 nBytes = 0;
	int nOutputs = 0, nSubmitted = 0, nPoolDrops = 0, nCadenceDrops = 0, nSampleSubmitted = 0;
	bool bOutputValid = true;
	encoder.setPacketCallback([&](const QByteArray &data, qint64 nPts)
	{
		const auto found = pending.find(nPts);
		if (found == pending.end())
		{
			bOutputValid = false;
			strError = QStringLiteral("Encoded output has unknown PTS: %1").arg(nPts);
			return;
		}
		if (found->second.bSample)
		{
			outputUs.push_back(std::chrono::duration<double, std::micro>(KClock::now() - found->second.started).count());
			nBytes += data.size();
		}
		pending.erase(found);
		++nOutputs;
		validator.enqueue(data, nPts);
	});
	const int nWarmSeconds = bSmoke ? 1 : 10, nSampleSeconds = bSmoke ? 2 : 60;
	const auto started = KClock::now();
	const auto sampleStart = started + std::chrono::seconds(nWarmSeconds);
	const auto finished = sampleStart + std::chrono::seconds(nSampleSeconds);
	KResourceSampler resourceSampler;
	resourceSampler.start(sampleStart);
	double fProcessStart = -1, fThreadStart = -1;
	int nIndex = 0;
	for (; KClock::now() < finished; ++nIndex)
	{
		const auto due = started + std::chrono::nanoseconds(static_cast<qint64>(nIndex) * 1000000000 / nFps);
		std::this_thread::sleep_until(due);
		const auto now = KClock::now();
		if (now >= finished)
			break;
		const int nDueIndex = static_cast<int>(std::chrono::duration<double>(now - started).count() * nFps);
		if (nDueIndex > nIndex)
		{
			nCadenceDrops += nDueIndex - nIndex;
			nIndex = nDueIndex;
		}
		const bool bSample = now >= sampleStart;
		if (bSample && fProcessStart < 0)
		{
			fProcessStart = CpuMs(false);
			fThreadStart = CpuMs(true);
		}
		source.draw(nIndex, nWidth, nHeight);
		const auto frameStart = KClock::now();
		pending.emplace(nIndex, KPending{frameStart, bSample});
		const qint64 nTimestamp = 1000000 + (static_cast<qint64>(nIndex) * 1000 + nFps / 2) / nFps;
		bool bSubmitted = false;
		if (bGpu)
		{
			const auto result = encoder.encodeGpuFrame(native, nTimestamp, nIndex == 0, &strError);
			bSubmitted = result == KH264Encoder::SubmittedGpuEncodeResult;
			if (result == KH264Encoder::BusyGpuEncodeResult)
				++nPoolDrops;
			else if (!bSubmitted)
				bOutputValid = false;
		}
		else
		{
			source.context->CopyResource(source.staging.Get(), source.texture.Get());
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (FAILED(source.context->Map(source.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
			{
				bOutputValid = false;
				strError = QStringLiteral("Synthetic source readback failed");
				break;
			}
			for (int nRow = 0; nRow < nHeight; ++nRow)
				std::memcpy(bgra.data() + static_cast<size_t>(nRow) * nWidth * 4,
					static_cast<const unsigned char *>(mapped.pData) + static_cast<size_t>(nRow) * mapped.RowPitch, nWidth * 4);
			source.context->Unmap(source.staging.Get(), 0);
			const uint8_t *sources[4] = {bgra.data(), nullptr, nullptr, nullptr};
			const int strides[4] = {nWidth * 4, 0, 0, 0};
			uint8_t *planes[4] = {y.data(), u.data(), v.data(), nullptr};
			const int planeStrides[4] = {nWidth, nWidth / 2, nWidth / 2, 0};
			bSubmitted = sws_scale(pSws, sources, strides, 0, nHeight, planes, planeStrides) == nHeight
				&& encoder.encodeI420Frame(y.data(), nWidth, u.data(), nWidth / 2, v.data(), nWidth / 2,
					nWidth, nHeight, nTimestamp, nIndex == 0, &strError);
			if (!bSubmitted)
				bOutputValid = false;
		}
		if (bSubmitted)
		{
			++nSubmitted;
			if (bSample)
			{
				++nSampleSubmitted;
				processingUs.push_back(std::chrono::duration<double, std::micro>(KClock::now() - frameStart).count());
			}
			if (encoder.lastSubmittedPts() != nIndex)
			{
				bOutputValid = false;
				strError = QStringLiteral("Submitted PTS mismatch: expected=%1 actual=%2")
					.arg(nIndex).arg(encoder.lastSubmittedPts());
			}
		}
		else
		{
			pending.erase(nIndex);
		}
		if (!bOutputValid || !validator.valid || pending.size() > 64)
			break;
	}
	const auto sampleFinished = KClock::now();
	const bool bFullDuration = sampleFinished >= finished;
	const double fProcessEnd = CpuMs(false), fThreadEnd = CpuMs(true);
	resourceSampler.stop();
	const auto resource = KResourceTraceLogger::snapshot();
	const int nPoolCapacity = encoder.gpuFramePoolCapacity();
	const int nPoolPeakInUse = encoder.gpuFramePoolPeakInUse();
	const bool bClosed = encoder.close(&strError);
	validator.finish();
	if (!validator.valid && strError.isEmpty())
		strError = validator.error();
	sws_freeContext(pSws);
	const bool bValid = bFullDuration && bClosed && bOutputValid && validator.valid && validator.nDecoded == nSubmitted
		&& nOutputs == nSubmitted && pending.empty() && !processingUs.empty()
		&& outputUs.size() == static_cast<size_t>(nSampleSubmitted);
	QJsonObject result;
	result[QStringLiteral("schema")] = 1;
	result[QStringLiteral("smokeOnly")] = bSmoke;
	result[QStringLiteral("mode")] = args[1];
	result[QStringLiteral("encoder")] = args[2];
	result[QStringLiteral("adapter")] = QString::fromWCharArray(source.adapter.Description);
	result[QStringLiteral("adapterLuid")] = QString::number(source.nLuid);
	result[QStringLiteral("driverVersionRaw")] = QString::number(source.driverVersion.QuadPart);
	result[QStringLiteral("ffmpegVersion")] = QString::fromLatin1(av_version_info());
	result[QStringLiteral("requestedBitrateKbps")] = 12000;
	result[QStringLiteral("width")] = nWidth;
	result[QStringLiteral("height")] = nHeight;
	result[QStringLiteral("fps")] = nFps;
	result[QStringLiteral("repetition")] = nRepetition;
	result[QStringLiteral("warmupSeconds")] = nWarmSeconds;
	result[QStringLiteral("sampleSeconds")] = nSampleSeconds;
	result[QStringLiteral("sampleWallSeconds")] = (std::max)(0.0,
		std::chrono::duration<double>(sampleFinished - sampleStart).count());
	result[QStringLiteral("elapsedWallSeconds")] = std::chrono::duration<double>(sampleFinished - started).count();
	result[QStringLiteral("processCpuIncludingValidatorMs")] = fProcessStart >= 0 && fProcessEnd >= 0
		? QJsonValue(fProcessEnd - fProcessStart) : QJsonValue(QJsonValue::Null);
	result[QStringLiteral("producerThreadCpuMs")] = fThreadStart >= 0 && fThreadEnd >= 0
		? QJsonValue(fThreadEnd - fThreadStart) : QJsonValue(QJsonValue::Null);
	result[QStringLiteral("processingCpuWall")] = Percentiles(processingUs);
	result[QStringLiteral("submitToEncodedOutputWall")] = Percentiles(outputUs);
	result[QStringLiteral("gpuExecutionMeasured")] = false;
	result[QStringLiteral("gpuConversionMicrobenchmark")] = ProfileConversionGpu(source, nWidth, nHeight);
	result[QStringLiteral("sampleSubmitted")] = nSampleSubmitted;
	result[QStringLiteral("submitted")] = nSubmitted;
	result[QStringLiteral("decoded")] = validator.nDecoded;
	result[QStringLiteral("outputs")] = nOutputs;
	result[QStringLiteral("validatorError")] = validator.error();
	result[QStringLiteral("validatorThreads")] = 4;
	result[QStringLiteral("validatorMaxPacketBytes")] = QString::number(validator.nMaxPacketBytes);
	result[QStringLiteral("validatorPeakQueuedBytes")] = QString::number(validator.nPeakQueuedBytes);
	result[QStringLiteral("pendingOutputs")] = static_cast<int>(pending.size());
	result[QStringLiteral("poolDrops")] = nPoolDrops;
	result[QStringLiteral("gpuFramePoolCapacity")] = nPoolCapacity;
	result[QStringLiteral("gpuFramePoolPeakInUse")] = nPoolPeakInUse;
	result[QStringLiteral("cadenceDropsIncludingWarmup")] = nCadenceDrops;
	result[QStringLiteral("sampleOutputBytes")] = QString::number(nBytes);
	result[QStringLiteral("measuredOutputBpsByMediaTime")] = static_cast<double>(nBytes) * 8 / nSampleSeconds;
	result[QStringLiteral("sampleReadbackBytes")] = QString::number(bGpu ? 0
		: static_cast<quint64>(nSampleSubmitted) * nWidth * nHeight * 4);
	result[QStringLiteral("privateBytesAtSampleEnd")] = QString::number(resource.nPrivateBytes);
	result[QStringLiteral("gpuDedicatedBytesAtSampleEnd")] = QString::number(resource.nGpuDedicatedBytes);
	result[QStringLiteral("gpuMemoryAvailable")] = resource.bGpuAvailable;
	result[QStringLiteral("resourceSampling")] = resourceSampler.result();
	result[QStringLiteral("gpuUtilizationMeasured")] = false;
	result[QStringLiteral("valid")] = bValid;
	result[QStringLiteral("error")] = strError;
	std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
	return bValid ? 0 : 1;
}
