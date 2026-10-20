#include "codec/h264encoder.h"
#include "codec/h264levelpolicy.h"

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <iterator>
#include <mutex>
#include <utility>
#include <QtCore/QDebug>
#include <QtCore/QDateTime>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <libyuv.h>

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"

namespace
{
	void ForwardEncoderLog(void *pContext, int nLevel, const char *pFormat, va_list arguments)
	{
		const bool bAmfUpdate = std::strncmp(pFormat, "amf_rate_update ", 16) == 0;
		if (bAmfUpdate && nLevel > AV_LOG_ERROR)
		{
			static std::atomic<qint64> s_nLastRateLogMs{0};
			const qint64 nNowMs = QDateTime::currentMSecsSinceEpoch();
			qint64 nPreviousMs = s_nLastRateLogMs.load(std::memory_order_relaxed);
			if (nNowMs >= nPreviousMs && nNowMs - nPreviousMs < 1000)
				return;
			if (!s_nLastRateLogMs.compare_exchange_strong(nPreviousMs, nNowMs, std::memory_order_relaxed))
				return;
		}
		va_list original;
		va_copy(original, arguments);
		av_log_default_callback(pContext, nLevel, pFormat, original);
		va_end(original);
		const bool bMftName = std::strncmp(pFormat, "MFT name:", 9) == 0;
		const bool bError = nLevel <= AV_LOG_ERROR;
		if (!bMftName && !bAmfUpdate && !bError)
			return;
		char buffer[512] = {};
		int nPrefix = 1;
		av_log_format_line2(pContext, nLevel, pFormat, arguments, buffer, sizeof(buffer), &nPrefix);
		if (bError)
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("ffmpeg_error"),
				QString::fromUtf8(buffer).trimmed());
		if (!bMftName && !bAmfUpdate)
			return;
		KLatencyTraceLogger::write(QStringLiteral("controlled"),
			bAmfUpdate ? QStringLiteral("amf_rate_update") : QStringLiteral("mf_encoder_selected"),
			QString::fromUtf8(buffer).trimmed());
	}

	constexpr std::int64_t kMinBitrate = 2'000'000;
	constexpr std::int64_t kMaxBitrate = 20'000'000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr int kMfScenarioDisplayRemoting = 1;
	constexpr int kMfRateControlLowDelayVbr = 4;
	constexpr char kSoftwareEncoderName[] = "libx264";

	bool ConfigureX264Bitrate(AVCodecContext *pContext, std::int64_t nBitrateBps)
	{
		// x264 cannot enable VBV after opening. Keep a 500 ms budget from startup.
		// FFmpeg passes VBV size to x264 in whole kbits; keep it enabled at 1 kbps too.
		const std::int64_t nBufferBits = std::max<std::int64_t>(1000, nBitrateBps / 2);
		if (nBitrateBps <= 0 || nBufferBits > (std::numeric_limits<int>::max)())
			return false;
		pContext->bit_rate = nBitrateBps;
		pContext->rc_max_rate = nBitrateBps;
		pContext->rc_buffer_size = static_cast<int>(nBufferBits);
		return true;
	}

	quint64 AdapterLuid(const LUID &luid)
	{
		return (static_cast<quint64>(static_cast<quint32>(luid.HighPart)) << 32) | luid.LowPart;
	}

	std::vector<KVideoAdapter> EnumerateVideoAdapters(QString *pError)
	{
		std::vector<KVideoAdapter> adapters;
		Microsoft::WRL::ComPtr<IDXGIFactory1> spFactory;
		HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&spFactory));
		if (FAILED(hr))
		{
			*pError = QStringLiteral("CreateDXGIFactory1 HRESULT=0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
			return adapters;
		}
		for (UINT nIndex = 0; ; ++nIndex)
		{
			Microsoft::WRL::ComPtr<IDXGIAdapter1> spAdapter;
			hr = spFactory->EnumAdapters1(nIndex, &spAdapter);
			if (hr == DXGI_ERROR_NOT_FOUND)
				break;
			DXGI_ADAPTER_DESC1 desc = {};
			if (SUCCEEDED(hr))
				hr = spAdapter->GetDesc1(&desc);
			if (FAILED(hr))
			{
				*pError = QStringLiteral("Enumerate adapter %1 HRESULT=0x%2")
					.arg(nIndex).arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
				break;
			}
			adapters.push_back({static_cast<int>(nIndex), desc.VendorId, desc.DeviceId,
				AdapterLuid(desc.AdapterLuid), QString::fromWCharArray(desc.Description),
				(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0});
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_adapter"),
				QStringLiteral("adapterIndex=%1 vendorId=%2 deviceId=%3 luid=%4 software=%5 gpu=%6")
					.arg(nIndex).arg(desc.VendorId).arg(desc.DeviceId).arg(AdapterLuid(desc.AdapterLuid))
					.arg((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0).arg(QString::fromWCharArray(desc.Description)));
		}
		return adapters;
	}

	int CreateVendorDevice(AVCodecContext *pContext, const QString &strEncoder, const KVideoAdapter &adapter)
	{
		AVBufferRef *pDevice = nullptr;
		const QByteArray deviceIndex = QByteArray::number(adapter.nIndex);
		int nResult = av_hwdevice_ctx_create(&pDevice, AV_HWDEVICE_TYPE_D3D11VA,
			deviceIndex.constData(), nullptr, 0);
		if (nResult < 0)
			return nResult;
		const auto *pHardware = reinterpret_cast<const AVHWDeviceContext *>(pDevice->data);
		const auto *pD3d = static_cast<const AVD3D11VADeviceContext *>(pHardware->hwctx);
		Microsoft::WRL::ComPtr<IDXGIDevice> spDxgiDevice;
		Microsoft::WRL::ComPtr<IDXGIAdapter> spAdapter;
		DXGI_ADAPTER_DESC descriptor = {};
		if (FAILED(pD3d->device->QueryInterface(IID_PPV_ARGS(&spDxgiDevice)))
			|| FAILED(spDxgiDevice->GetAdapter(&spAdapter)) || FAILED(spAdapter->GetDesc(&descriptor))
			|| descriptor.VendorId != adapter.nVendorId
			|| AdapterLuid(descriptor.AdapterLuid) != adapter.nLuid)
		{
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_device_mismatch"),
				QStringLiteral("expectedVendor=%1 expectedLuid=%2 actualVendor=%3 actualLuid=%4 adapterIndex=%5")
					.arg(adapter.nVendorId).arg(adapter.nLuid).arg(descriptor.VendorId)
					.arg(AdapterLuid(descriptor.AdapterLuid)).arg(adapter.nIndex));
			av_buffer_unref(&pDevice);
			return AVERROR_EXTERNAL;
		}
		LARGE_INTEGER version = {};
		const HRESULT hVersion = spAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
		const QString strDetails = QStringLiteral("encoder=%1 gpu=%2 vendorId=%3 deviceId=%4 driverVersionRaw=%5 versionResult=%6 adapterIndex=%7 luid=%8 captureAffinity=cpu_upload")
			.arg(strEncoder,
				QString::fromWCharArray(descriptor.Description))
			.arg(descriptor.VendorId).arg(descriptor.DeviceId).arg(version.QuadPart).arg(hVersion)
			.arg(adapter.nIndex).arg(adapter.nLuid);
		qInfo().noquote() << "h264_device_selected" << strDetails;
		KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_device_selected"), strDetails);
		KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_device_selected"), strDetails);
		if (strEncoder != QStringLiteral("h264_qsv"))
			pContext->hw_device_ctx = pDevice;
		else
		{
			nResult = av_hwdevice_ctx_create_derived(&pContext->hw_device_ctx, AV_HWDEVICE_TYPE_QSV, pDevice, 0);
			av_buffer_unref(&pDevice);
		}
		return nResult;
	}
}

KH264Encoder::KH264Encoder(KVideoEncoderPreference preference, KNvencReconfiguration nvencReconfiguration)
	: m_preference(preference)
	, m_nvencReconfiguration(nvencReconfiguration)
{
}

KH264Encoder::~KH264Encoder()
{
	QString strIgnoredError;
	close(&strIgnoredError);
}

bool KH264Encoder::openStream(int nWidth,
	int nHeight,
	int nFps,
	const DataCallback &callback,
	QString *pErrorMessage)
{
	return openStream(nWidth, nHeight, nFps, 0, callback, pErrorMessage);
}

bool KH264Encoder::openStream(int nWidth,
	int nHeight,
	int nFps,
	int nBitrateKbps,
	const DataCallback &callback,
	QString *pErrorMessage)
{
	if (isOpen())
		close(nullptr);

	if (nWidth < 2 || nHeight < 2 || nFps <= 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 encoder open failed: invalid parameter");
		return false;
	}

	if (!callback)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 stream callback is empty");
		return false;
	}
	if (m_nvencReconfiguration.bOverride && m_preference != NvencVideoEncoderPreference)
	{
		if (pErrorMessage)
			*pErrorMessage = QStringLiteral("Explicit NVENC reconfiguration requires the NVENC backend");
		return false;
	}

	m_nEncodedWidth = nWidth & ~1;
	m_nEncodedHeight = nHeight & ~1;
	m_nFps = nFps;
	m_nBitrateKbps = nBitrateKbps;
	m_nFirstTimestampMs = 0;
	m_nLastPts = -1;
	m_dataCallback = callback;
	m_strEncoderName.clear();
	m_strFallbackReason.clear();

	QString strEnumerationError;
	std::vector<KVideoAdapter> adapters;
	if (m_preference == AutoVideoEncoderPreference || m_preference == QuickSyncVideoEncoderPreference
		|| m_preference == AmfVideoEncoderPreference || m_preference == NvencVideoEncoderPreference)
		adapters = EnumerateVideoAdapters(&strEnumerationError);
	const auto candidates = BuildVideoEncoderCandidates(m_preference, adapters);
	QStringList failures;
	const bool bSelected = TryVideoEncoderCandidates(candidates,
		[this](const KVideoEncoderCandidate &candidate, QString *pError)
		{
			m_candidate = candidate;
			const QString strDetails = QStringLiteral("requested=%1 encoder=%2 adapterIndex=%3 vendorId=%4 deviceId=%5 luid=%6 gpu=%7")
				.arg(VideoEncoderPreferenceName(m_preference)).arg(candidate.strEncoder).arg(candidate.adapter.nIndex)
				.arg(candidate.adapter.nVendorId).arg(candidate.adapter.nDeviceId)
				.arg(candidate.adapter.nLuid).arg(candidate.adapter.strName);
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_candidate"), strDetails);
			const QByteArray name = candidate.strEncoder.toLatin1();
			const bool bOpened = openCodec(name.constData(), candidate.strEncoder != QStringLiteral("libx264"), pError);
			if (!bOpened)
			{
				// Release the failed context and its device before trying another adapter.
				avcodec_free_context(&m_pCodecContext);
				KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_candidate_failed"),
					QStringLiteral("%1 error=%2").arg(strDetails, *pError));
			}
			return bOpened;
		}, &m_candidate, &failures);
	if (!strEnumerationError.isEmpty())
	{
		failures.prepend(strEnumerationError);
		KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_enumeration_failed"), strEnumerationError);
	}
	if (!bSelected)
	{
		if (pErrorMessage)
			*pErrorMessage = failures.isEmpty() ? QStringLiteral("No matching hardware adapter for %1")
				.arg(VideoEncoderPreferenceName(m_preference))
				: failures.join(QStringLiteral("; "));
		release();
		return false;
	}
	if (m_preference == AutoVideoEncoderPreference && candidates.size() == 1)
		failures.prepend(QStringLiteral("No supported QSV/AMF/NVENC hardware adapter"));
	m_strFallbackReason = failures.join(QStringLiteral("; "));
	KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("encoder_selection"),
		QStringLiteral("requested=%1 actual=%2 adapterIndex=%3 fallback=%4 reason=%5")
			.arg(VideoEncoderPreferenceName(m_preference)).arg(m_strEncoderName).arg(m_candidate.adapter.nIndex)
			.arg(m_strFallbackReason.isEmpty() ? 0 : 1).arg(m_strFallbackReason));

	m_pFrame = av_frame_alloc();
	if (m_pFrame == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create video frame failed");
		release();
		return false;
	}

	m_pFrame->format = m_pCodecContext->pix_fmt;
	m_pFrame->width = m_pCodecContext->width;
	m_pFrame->height = m_pCodecContext->height;

	const int nFrameResult = av_frame_get_buffer(m_pFrame, 32);
	if (nFrameResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Allocate video frame buffer failed"), nFrameResult);
		release();
		return false;
	}

	m_pPacket = av_packet_alloc();
	if (m_pPacket == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create encoded packet failed");
		release();
		return false;
	}

	m_pSwsContext = sws_getContext(m_nEncodedWidth,
		m_nEncodedHeight,
		AV_PIX_FMT_BGRA,
		m_nEncodedWidth,
		m_nEncodedHeight,
		AV_PIX_FMT_NV12,
		SWS_FAST_BILINEAR,
		nullptr,
		nullptr,
		nullptr);
	if (m_pSwsContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create BGRA to NV12 converter failed");
		release();
		return false;
	}

	m_bOpen = true;
	return true;
}

bool KH264Encoder::encodeBgraFrame(const unsigned char *pBgraData,
	int nWidth,
	int nHeight,
	qint64 nTimestampMs,
	QString *pErrorMessage)
{
	if (!isOpen() || pBgraData == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 encoder is not open");
		return false;
	}

	if (nWidth < m_nEncodedWidth || nHeight < m_nEncodedHeight)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Input frame size is smaller than encoder size");
		return false;
	}

	const int nWritableResult = av_frame_make_writable(m_pFrame);
	if (nWritableResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Make video frame writable failed"), nWritableResult);
		return false;
	}

	const unsigned char *pSrcSlice[] = { pBgraData, nullptr, nullptr, nullptr };
	const int nSrcStride[] = { nWidth * 4, 0, 0, 0 };
	const int nScaleResult = sws_scale(m_pSwsContext,
		pSrcSlice,
		nSrcStride,
		0,
		m_nEncodedHeight,
		m_pFrame->data,
		m_pFrame->linesize);
	if (nScaleResult != m_nEncodedHeight)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Convert BGRA frame to NV12 failed");
		return false;
	}

	if (!prepareFrame(nTimestampMs, false, pErrorMessage))
		return false;

	return submitFrame(m_pFrame, pErrorMessage);
}

bool KH264Encoder::encodeI420Frame(const unsigned char *pYData,
	int nStrideY,
	const unsigned char *pUData,
	int nStrideU,
	const unsigned char *pVData,
	int nStrideV,
	int nWidth,
	int nHeight,
	qint64 nTimestampMs,
	bool bForceKeyFrame,
	QString *pErrorMessage)
{
	if (!isOpen() || pYData == nullptr || pUData == nullptr || pVData == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 encoder is not open");
		return false;
	}

	if (nWidth < m_nEncodedWidth || nHeight < m_nEncodedHeight)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Input frame size is smaller than encoder size");
		return false;
	}

	const int nWritableResult = av_frame_make_writable(m_pFrame);
	if (nWritableResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Make video frame writable failed"), nWritableResult);
		return false;
	}

	const int nConvertResult = libyuv::I420ToNV12(pYData,
		nStrideY,
		pUData,
		nStrideU,
		pVData,
		nStrideV,
		m_pFrame->data[0],
		m_pFrame->linesize[0],
		m_pFrame->data[1],
		m_pFrame->linesize[1],
		m_nEncodedWidth,
		m_nEncodedHeight);
	if (nConvertResult != 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Convert I420 frame to NV12 failed");
		return false;
	}

	if (!prepareFrame(nTimestampMs, bForceKeyFrame, pErrorMessage))
		return false;

	return submitFrame(m_pFrame, pErrorMessage);
}

bool KH264Encoder::setLevelLimit(int nLevel)
{
	if (isOpen() || !KH264LevelPolicy::isSupported(nLevel))
		return false;
	m_nLevelLimit = nLevel;
	return true;
}

void KH264Encoder::setBitrateKbps(int nBitrateKbps)
{
	if (nBitrateKbps <= 0 || nBitrateKbps == m_nBitrateKbps)
		return;
	const std::int64_t nBitrateBps = static_cast<std::int64_t>(nBitrateKbps) * kBitsPerKilobit;
	if (m_nLevelLimit && !KH264LevelPolicy::fits(m_nLevelLimit, m_nEncodedWidth, m_nEncodedHeight,
		m_nFps, nBitrateBps, nBitrateBps / 2, 1))
	{
		qWarning() << "Rejected bitrate outside H.264 level constraint" << m_nLevelLimit << nBitrateBps;
		return;
	}
	if (m_pCodecContext != nullptr && m_strEncoderName == QString::fromLatin1(kSoftwareEncoderName))
	{
		if (!ConfigureX264Bitrate(m_pCodecContext, nBitrateBps))
		{
			qWarning() << "Rejected x264 requested bitrate: VBV buffer exceeds integer range" << nBitrateKbps;
			return;
		}
	}
	else if (m_pCodecContext != nullptr && (m_strEncoderName == QStringLiteral("h264_qsv")
		|| m_strEncoderName == QStringLiteral("h264_amf") || m_strEncoderName == QStringLiteral("h264_nvenc")))
	{
		if (nBitrateBps / 4 > (std::numeric_limits<int>::max)()
			|| (m_strEncoderName == QStringLiteral("h264_nvenc")
				&& nBitrateBps > (std::numeric_limits<std::uint32_t>::max)()))
		{
			qWarning() << "Rejected vendor requested bitrate: buffer exceeds integer range" << nBitrateKbps;
			return;
		}
		m_pCodecContext->bit_rate = nBitrateBps;
		m_pCodecContext->rc_max_rate = nBitrateBps;
		// QSV Reset cannot resize the buffer allocated at initialization.
		if (m_strEncoderName != QStringLiteral("h264_qsv"))
			m_pCodecContext->rc_buffer_size = static_cast<int>(std::max<std::int64_t>(1000, nBitrateBps / 4));
	}
	else if (m_pCodecContext != nullptr)
	{
		if (nBitrateBps > (std::numeric_limits<std::uint32_t>::max)())
		{
			qWarning() << "Rejected MF requested bitrate: exceeds UINT32 range" << nBitrateKbps;
			return;
		}
		if (!m_bReportedUnsupportedRate)
		{
			m_bReportedUnsupportedRate = true;
			KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("h264_rate_update_unsupported"),
				QStringLiteral("encoder=h264_mf requestedBitrateBps=%1 initialBitrateBps=%2")
					.arg(nBitrateBps).arg(m_pCodecContext->bit_rate));
			qWarning() << "h264_mf does not apply runtime bitrate updates; initial configuration is retained";
		}
	}
	m_nBitrateKbps = nBitrateKbps;
}

void KH264Encoder::setDataCallback(const DataCallback &callback)
{
	m_dataCallback = callback;
}

void KH264Encoder::setPacketCallback(const PacketCallback &callback)
{
	m_packetCallback = callback;
}

std::int64_t KH264Encoder::lastSubmittedPts() const
{
	return m_nLastPts;
}

bool KH264Encoder::submitFrame(AVFrame *pFrame, QString *pErrorMessage)
{
	int nResult = avcodec_send_frame(m_pCodecContext, pFrame);
	if (nResult == AVERROR(EAGAIN))
	{
		if (!writePacket(pErrorMessage))
			return false;
		nResult = avcodec_send_frame(m_pCodecContext, pFrame);
	}
	if (nResult < 0 && !(pFrame == nullptr && nResult == AVERROR_EOF))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Submit H.264 frame failed"), nResult);
		return false;
	}
	return writePacket(pErrorMessage);
}

bool KH264Encoder::close(QString *pErrorMessage)
{
	bool bResult = true;

	if (m_pCodecContext != nullptr)
	{
		bResult = submitFrame(nullptr, pErrorMessage);
	}

	release();
	return bResult;
}

bool KH264Encoder::isOpen() const
{
	return m_bOpen && m_pCodecContext != nullptr;
}

int KH264Encoder::encodedWidth() const
{
	return m_nEncodedWidth;
}

int KH264Encoder::encodedHeight() const
{
	return m_nEncodedHeight;
}

QByteArray KH264Encoder::codecHeaderData() const
{
	if (m_pCodecContext == nullptr
		|| m_pCodecContext->extradata == nullptr
		|| m_pCodecContext->extradata_size <= 0)
	{
		return QByteArray();
	}

	return QByteArray(reinterpret_cast<const char *>(m_pCodecContext->extradata),
		m_pCodecContext->extradata_size);
}

QString KH264Encoder::encoderName() const
{
	return m_strEncoderName;
}

QString KH264Encoder::fallbackReason() const
{
	return m_strFallbackReason;
}

bool KH264Encoder::openCodec(const char *pCodecName, bool bHardware, QString *pErrorMessage)
{
	static std::once_flag logCallbackOnce;
	std::call_once(logCallbackOnce, [] { av_log_set_callback(ForwardEncoderLog); });
	KLatencyTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("ffmpeg_build"),
		QStringLiteral("encoder=%1 version=%2").arg(QString::fromLatin1(pCodecName),
			QString::fromUtf8(av_version_info())));
	const AVCodec *pCodec = avcodec_find_encoder_by_name(pCodecName);
	if (pCodec == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("FFmpeg encoder %1 not found").arg(QString::fromLatin1(pCodecName));
		return false;
	}

	m_pCodecContext = avcodec_alloc_context3(pCodec);
	if (m_pCodecContext == nullptr)
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = QStringLiteral("Create %1 codec context failed")
				.arg(QString::fromLatin1(pCodecName));
		}
		return false;
	}

	m_pCodecContext->codec_id = pCodec->id;
	m_pCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
	m_pCodecContext->width = m_nEncodedWidth;
	m_pCodecContext->height = m_nEncodedHeight;
	m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;
	m_pCodecContext->time_base = AVRational{ 1, m_nFps };
	m_pCodecContext->framerate = AVRational{ m_nFps, 1 };
	m_pCodecContext->gop_size = m_nFps * 2;
	m_pCodecContext->max_b_frames = 0;
	m_pCodecContext->thread_count = bHardware ? 1 : 0;
	if (!bHardware)
	{
		m_pCodecContext->thread_type = FF_THREAD_SLICE;
		m_pCodecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
	}
	const std::int64_t nRequestedBitrate = static_cast<std::int64_t>(m_nBitrateKbps) * kBitsPerKilobit;
	m_pCodecContext->bit_rate = nRequestedBitrate > 0
		? nRequestedBitrate
		: std::clamp(
			static_cast<std::int64_t>(m_nEncodedWidth) * m_nEncodedHeight * m_nFps / 4,
			kMinBitrate,
			kMaxBitrate);

	const bool bNvenc = std::strcmp(pCodecName, "h264_nvenc") == 0;
	const bool bVendor = std::strcmp(pCodecName, "h264_qsv") == 0 || std::strcmp(pCodecName, "h264_amf") == 0 || bNvenc;
	if (bVendor)
	{
		const bool bAmf = std::strcmp(pCodecName, "h264_amf") == 0;
		// Reserve QSV's fixed allocation for the session level budget, not its
		// small startup target. Reset cannot grow it when congestion control ramps up.
		const std::int64_t nBufferBudgetBps = (bAmf || bNvenc) ? m_pCodecContext->bit_rate
			: std::max<std::int64_t>(m_pCodecContext->bit_rate, m_nLevelLimit
				? KH264LevelPolicy::maximumBitrate(m_nLevelLimit) : kMaxBitrate);
		const std::int64_t nBufferBits = std::max<std::int64_t>(1000, nBufferBudgetBps / 4);
		if (nBufferBits > (std::numeric_limits<int>::max)()
			|| (bNvenc && m_pCodecContext->bit_rate > (std::numeric_limits<std::uint32_t>::max)()))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Requested vendor bitrate exceeds buffer range");
			avcodec_free_context(&m_pCodecContext);
			return false;
		}
		m_pCodecContext->rc_max_rate = m_pCodecContext->bit_rate;
		m_pCodecContext->rc_buffer_size = static_cast<int>(nBufferBits);
		m_pCodecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
		// QSV CBR rate changes require HRD conformance to be disabled. In this
		// FFmpeg baseline, strict < normal maps to NalHrdConformance=OFF.
		if (!bAmf && !bNvenc)
			m_pCodecContext->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
		const int nDeviceResult = CreateVendorDevice(m_pCodecContext, QString::fromLatin1(pCodecName), m_candidate.adapter);
		if (nDeviceResult < 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Create vendor hardware device failed"), nDeviceResult);
			avcodec_free_context(&m_pCodecContext);
			return false;
		}
		const std::pair<const char *, const char *> amfOptions[] = {
			{ "usage", "ultralowlatency" }, { "quality", "speed" }, { "rc", "cbr" },
			{ "profile", "constrained_baseline" }, { "async_depth", "1" }, { "forced_idr", "1" }
		};
		// The public FFmpeg and VPL constrained-baseline values are both 66 | (1 << 9).
		const QByteArray qsvProfile = QByteArray::number(AV_PROFILE_H264_CONSTRAINED_BASELINE);
		const std::pair<const char *, const char *> qsvOptions[] = {
			{ "preset", "veryfast" }, { "profile", qsvProfile.constData() }, { "async_depth", "1" },
			{ "look_ahead", "0" }, { "forced_idr", "1" }
		};
		const std::pair<const char *, const char *> nvencOptions[] = {
			{ "preset", "p4" }, { "tune", "ull" }, { "rc", "cbr" }, { "profile", "baseline" },
			{ "rc-lookahead", "0" }, { "zerolatency", "1" }, { "delay", "0" }, { "forced-idr", "1" }
		};
		const auto *pOptions = bNvenc ? nvencOptions : (bAmf ? amfOptions : qsvOptions);
		const bool bNvencPolicySupported = bNvenc
			&& av_opt_find(m_pCodecContext->priv_data, "bitrate-reconfig-reset", nullptr, 0, 0)
			&& av_opt_find(m_pCodecContext->priv_data, "bitrate-reconfig-idr", nullptr, 0, 0);
		if (bNvenc && (m_nvencReconfiguration.bOverride || bNvencPolicySupported))
		{
			const bool bReset = m_nvencReconfiguration.bOverride ? m_nvencReconfiguration.bReset : false;
			const bool bForceIdr = m_nvencReconfiguration.bOverride ? m_nvencReconfiguration.bForceIdr : false;
			const std::pair<const char *, int> reconfigurationOptions[] = {
				{ "bitrate-reconfig-reset", bReset },
				{ "bitrate-reconfig-idr", bForceIdr }
			};
			for (const auto &option : reconfigurationOptions)
			{
				const int nResult = av_opt_set_int(m_pCodecContext->priv_data, option.first, option.second, 0);
				if (nResult < 0)
				{
					if (pErrorMessage)
						*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Set NVENC option %1 failed")
							.arg(QString::fromLatin1(option.first)), nResult);
					avcodec_free_context(&m_pCodecContext);
					return false;
				}
			}
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("nvenc_reconfiguration_policy"),
				QStringLiteral("reset=%1 forceIdr=%2 source=%3 apiAcceptedNotMeasured=1")
					.arg(bReset).arg(bForceIdr).arg(m_nvencReconfiguration.bOverride
						? QStringLiteral("explicit") : QStringLiteral("project_default")));
		}
		else if (bNvenc)
		{
			KSessionTraceLogger::write(QStringLiteral("controlled"), QStringLiteral("nvenc_reconfiguration_policy"),
				QStringLiteral("reset=1 forceIdr=1 source=library_default reason=options_unavailable"));
		}
		const size_t nOptionCount = bNvenc ? std::size(nvencOptions) : (bAmf ? std::size(amfOptions) : std::size(qsvOptions));
		for (size_t nIndex = 0; nIndex < nOptionCount; ++nIndex)
		{
			const int nResult = av_opt_set(m_pCodecContext->priv_data, pOptions[nIndex].first,
				pOptions[nIndex].second, 0);
			if (nResult < 0)
			{
				if (pErrorMessage != nullptr)
					*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Set encoder option %1 failed")
						.arg(QString::fromLatin1(pOptions[nIndex].first)), nResult);
				avcodec_free_context(&m_pCodecContext);
				return false;
			}
		}
	}
	else if (bHardware)
	{
		if (m_pCodecContext->bit_rate > (std::numeric_limits<std::uint32_t>::max)())
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Requested MF bitrate exceeds UINT32 range");
			avcodec_free_context(&m_pCodecContext);
			return false;
		}
		av_opt_set_int(m_pCodecContext->priv_data, "hw_encoding", 1, 0);
		av_opt_set_int(m_pCodecContext->priv_data, "scenario", kMfScenarioDisplayRemoting, 0);
		av_opt_set_int(m_pCodecContext->priv_data, "rate_control", kMfRateControlLowDelayVbr, 0);
	}
	else
	{
		if (!ConfigureX264Bitrate(m_pCodecContext, m_pCodecContext->bit_rate))
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Requested x264 bitrate exceeds VBV buffer range");
			avcodec_free_context(&m_pCodecContext);
			return false;
		}
		av_opt_set(m_pCodecContext->priv_data, "preset", "ultrafast", 0);
		av_opt_set(m_pCodecContext->priv_data, "tune", "zerolatency", 0);
		av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
	}

	if (m_nLevelLimit)
	{
		if (!KH264LevelPolicy::fits(m_nLevelLimit, m_nEncodedWidth, m_nEncodedHeight, m_nFps,
			m_pCodecContext->bit_rate, m_pCodecContext->rc_buffer_size, 1))
		{
			if (pErrorMessage)
				*pErrorMessage = QStringLiteral("Video configuration exceeds H.264 level %1").arg(m_nLevelLimit);
			avcodec_free_context(&m_pCodecContext);
			return false;
		}
		m_pCodecContext->level = m_nLevelLimit;
		m_pCodecContext->refs = 1;
	}
	const int nOpenResult = avcodec_open2(m_pCodecContext, pCodec, nullptr);
	if (nOpenResult < 0)
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = ffmpegErrorMessage(
				QStringLiteral("Open %1 failed").arg(QString::fromLatin1(pCodecName)),
				nOpenResult);
		}
		avcodec_free_context(&m_pCodecContext);
		return false;
	}

	m_strEncoderName = QString::fromLatin1(pCodecName);
	return true;
}

bool KH264Encoder::prepareFrame(qint64 nTimestampMs, bool bForceKeyFrame, QString *)
{
	if (m_nFirstTimestampMs <= 0)
		m_nFirstTimestampMs = nTimestampMs;

	std::int64_t nPts = static_cast<std::int64_t>(
		((nTimestampMs - m_nFirstTimestampMs) * static_cast<qint64>(m_nFps) + 500) / 1000);
	if (nPts <= m_nLastPts)
		nPts = m_nLastPts + 1;

	m_pFrame->pts = nPts;
	m_pFrame->pict_type = bForceKeyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
	m_nLastPts = nPts;
	return true;
}

bool KH264Encoder::writePacket(QString *pErrorMessage)
{
	while (m_pCodecContext != nullptr && m_pPacket != nullptr)
	{
		const int nResult = avcodec_receive_packet(m_pCodecContext, m_pPacket);
		if (nResult == AVERROR(EAGAIN) || nResult == AVERROR_EOF)
			return true;
		if (nResult < 0)
		{
			if (pErrorMessage != nullptr && pErrorMessage->isEmpty())
				*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Receive H.264 packet failed"), nResult);
			return false;
		}

		if (m_dataCallback || m_packetCallback)
		{
			const QByteArray packet(reinterpret_cast<const char *>(m_pPacket->data), m_pPacket->size);
			if (m_dataCallback)
				m_dataCallback(packet);
			if (m_packetCallback)
				m_packetCallback(packet, m_pPacket->pts);
		}
		av_packet_unref(m_pPacket);
	}

	return true;
}

void KH264Encoder::release()
{
	if (m_pPacket != nullptr)
	{
		av_packet_free(&m_pPacket);
		m_pPacket = nullptr;
	}

	if (m_pFrame != nullptr)
	{
		av_frame_free(&m_pFrame);
		m_pFrame = nullptr;
	}

	if (m_pSwsContext != nullptr)
	{
		sws_freeContext(m_pSwsContext);
		m_pSwsContext = nullptr;
	}

	if (m_pCodecContext != nullptr)
	{
		avcodec_free_context(&m_pCodecContext);
		m_pCodecContext = nullptr;
	}

	m_dataCallback = DataCallback();
	m_packetCallback = PacketCallback();
	m_nEncodedWidth = 0;
	m_nEncodedHeight = 0;
	m_nBitrateKbps = 0;
	m_nFirstTimestampMs = 0;
	m_nLastPts = -1;
	m_strEncoderName.clear();
	m_strFallbackReason.clear();
	m_bOpen = false;
	m_bReportedUnsupportedRate = false;
}

QString KH264Encoder::ffmpegErrorMessage(const QString &strPrefix, int nErrorCode)
{
	char szBuffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(nErrorCode, szBuffer, sizeof(szBuffer));
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromLocal8Bit(szBuffer));
}
