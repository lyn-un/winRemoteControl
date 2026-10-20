#include "codec/h264encoder.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

class KH264EncoderTestAccess
{
public:
	static const AVCodecContext *context(const KH264Encoder &encoder)
	{
		return encoder.m_pCodecContext;
	}
	static std::int64_t lastPts(const KH264Encoder &encoder)
	{
		return encoder.m_nLastPts;
	}
};

namespace
{
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 30;
constexpr int kStageFrames = 300;
constexpr int kWarmupFrames = 60;

struct KStageResult
{
	std::uint64_t nBytes = 0;
	std::uint64_t nSquaredError = 0;
	int nMeasuredFrames = 0;

	double bitrateKbps() const
	{
		return nBytes * 8.0 * kFps / (kStageFrames - kWarmupFrames) / 1000.0;
	}
	double psnr() const
	{
		return nSquaredError == 0 ? 100.0 : 10.0 * std::log10(
			255.0 * 255.0 * kWidth * kHeight * nMeasuredFrames / nSquaredError);
	}
};

class KDecodeReference
{
public:
	KDecodeReference() = default;
	KDecodeReference(const KDecodeReference &) = delete;
	KDecodeReference &operator=(const KDecodeReference &) = delete;
	~KDecodeReference()
	{
		av_packet_free(&m_pPacket);
		av_frame_free(&m_pFrame);
		avcodec_free_context(&m_pContext);
	}
	bool initialize()
	{
		const AVCodec *pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!pCodec)
			return false;
		m_pContext = avcodec_alloc_context3(pCodec);
		m_pFrame = av_frame_alloc();
		m_pPacket = av_packet_alloc();
		if (!m_pContext || !m_pFrame || !m_pPacket)
			return false;
		m_pContext->thread_count = 1;
		return avcodec_open2(m_pContext, pCodec, nullptr) >= 0;
	}
	bool decode(const QByteArray &data, const std::vector<unsigned char> &reference,
		bool bMeasure, KStageResult *pResult)
	{
		av_packet_unref(m_pPacket);
		if (av_new_packet(m_pPacket, static_cast<int>(data.size())) < 0)
			return false;
		std::copy(data.begin(), data.end(), m_pPacket->data);
		if (avcodec_send_packet(m_pContext, m_pPacket) < 0
			|| avcodec_receive_frame(m_pContext, m_pFrame) < 0)
			return false;
		if (m_pFrame->width != kWidth || m_pFrame->height != kHeight
			|| m_pFrame->format != AV_PIX_FMT_YUV420P)
			return false;
		if (bMeasure)
		{
			pResult->nBytes += data.size();
			++pResult->nMeasuredFrames;
			for (int y = 0; y < kHeight; ++y)
				for (int x = 0; x < kWidth; ++x)
				{
					const int nDifference = reference[y * kWidth + x]
						- m_pFrame->data[0][y * m_pFrame->linesize[0] + x];
					pResult->nSquaredError += nDifference * nDifference;
				}
		}
		av_frame_unref(m_pFrame);
		return avcodec_receive_frame(m_pContext, m_pFrame) == AVERROR(EAGAIN);
	}

private:
	AVCodecContext *m_pContext = nullptr;
	AVFrame *m_pFrame = nullptr;
	AVPacket *m_pPacket = nullptr;
};

bool RunSequence(const char *pName, const std::array<int, 3> &rates,
	std::array<KStageResult, 3> *pResults)
{
	QByteArray encoded;
	KH264Encoder encoder(LibX264VideoEncoderPreference);
	KDecodeReference decoder;
	QString strError;
	if (!decoder.initialize() || !encoder.openStream(kWidth, kHeight, kFps, rates[0],
		[&encoded](const QByteArray &packet) { encoded.append(packet); }, &strError))
	{
		std::cerr << "Initialization failed: " << strError.toStdString() << '\n';
		return false;
	}
	std::vector<unsigned char> texture(kWidth * kHeight);
	std::uint32_t nSeed = 0x6d2b79f5;
	for (auto &pixel : texture)
	{
		nSeed ^= nSeed << 13;
		nSeed ^= nSeed >> 17;
		nSeed ^= nSeed << 5;
		pixel = static_cast<unsigned char>(16 + nSeed % 220);
	}
	std::vector<unsigned char> luma(kWidth * kHeight);
	std::vector<unsigned char> chroma(kWidth * kHeight / 4, 128);
	const AVCodecContext *pOriginalContext = KH264EncoderTestAccess::context(encoder);
	for (int nFrame = 0; nFrame < kStageFrames * 3; ++nFrame)
	{
		const int nStage = nFrame / kStageFrames;
		const int nLocalFrame = nFrame % kStageFrames;
		if (nLocalFrame == 0)
		{
			const auto nPreviousPts = KH264EncoderTestAccess::lastPts(encoder);
			encoder.setBitrateKbps(rates[nStage]);
			if (KH264EncoderTestAccess::context(encoder) != pOriginalContext
				|| KH264EncoderTestAccess::lastPts(encoder) != nPreviousPts)
				return false;
		}
		// Repeat the same textured animation in each stage and every control run.
		for (int y = 0; y < kHeight; ++y)
			for (int x = 0; x < kWidth; ++x)
				luma[y * kWidth + x] = texture[((y + nLocalFrame * 3) % kHeight) * kWidth
					+ (x + nLocalFrame * 7) % kWidth];
		encoded.clear();
		if (!encoder.encodeI420Frame(luma.data(), kWidth, chroma.data(), kWidth / 2,
			chroma.data(), kWidth / 2, kWidth, kHeight, 1000 + nFrame * 1000LL / kFps,
			nFrame == 0, &strError) || encoded.isEmpty()
			|| !decoder.decode(encoded, luma, nLocalFrame >= kWarmupFrames, &(*pResults)[nStage]))
		{
			std::cerr << pName << " encode/decode failed frame=" << nFrame << ' '
				<< strError.toStdString() << '\n';
			return false;
		}
	}
	for (int nStage = 0; nStage < 3; ++nStage)
	{
		const auto &result = (*pResults)[nStage];
		std::cout << pName << " stage=" << nStage << " requestedKbps=" << rates[nStage]
			<< " measuredKbps=" << result.bitrateKbps() << " lumaPsnrDb=" << result.psnr()
			<< " measuredFrames=" << result.nMeasuredFrames << std::endl;
	}
	encoded.clear();
	return encoder.close(&strError) && encoded.isEmpty();
}

bool TestBitrateParameterBounds()
{
	KH264Encoder encoder(LibX264VideoEncoderPreference);
	QString strError;
	if (!encoder.openStream(kWidth, kHeight, kFps, 0, [](const QByteArray &) {}, &strError))
		return false;
	const auto *pContext = KH264EncoderTestAccess::context(encoder);
	const auto nDefaultBitrate = pContext->bit_rate;
	if (nDefaultBitrate != kWidth * kHeight * kFps / 4
		|| pContext->rc_max_rate != nDefaultBitrate || pContext->rc_buffer_size != nDefaultBitrate / 2)
		return false;
	encoder.setBitrateKbps(1000);
	encoder.setBitrateKbps(1);
	if (pContext->bit_rate != 1000 || pContext->rc_max_rate != 1000 || pContext->rc_buffer_size != 1000)
		return false;
	encoder.setBitrateKbps(1000);
	for (int nRate : {1000, 0, -1, (std::numeric_limits<int>::max)()})
	{
		encoder.setBitrateKbps(nRate);
		if (KH264EncoderTestAccess::context(encoder) != pContext || pContext->bit_rate != 1000000
			|| pContext->rc_max_rate != 1000000 || pContext->rc_buffer_size != 500000)
			return false;
	}
	const int nMaximumKbps = (std::numeric_limits<int>::max)() / 500;
	encoder.setBitrateKbps(nMaximumKbps);
	if (pContext->rc_buffer_size != nMaximumKbps * 500
		|| pContext->bit_rate != static_cast<std::int64_t>(nMaximumKbps) * 1000)
		return false;
	encoder.setBitrateKbps(nMaximumKbps + 1);
	if (pContext->rc_buffer_size != nMaximumKbps * 500)
		return false;
	encoder.setBitrateKbps(1000);
	if (!encoder.close(&strError))
		return false;
	return !encoder.openStream(kWidth, kHeight, kFps, (std::numeric_limits<int>::max)(),
		[](const QByteArray &) {}, &strError) && !strError.isEmpty() && !encoder.isOpen();
}
} // namespace

bool TestH264BitrateOutput()
{
	KH264Encoder incompatible(LibX264VideoEncoderPreference, {true, false, false});
	QString strError;
	if (incompatible.openStream(320, 240, 30, 1000, [](const QByteArray &) {}, &strError)
		|| incompatible.isOpen() || !strError.contains(QStringLiteral("requires the NVENC backend")))
		return false;
	std::array<KStageResult, 3> dynamic, low, high;
	if (!RunSequence("dynamic", {1000, 6000, 1000}, &dynamic)
		|| !RunSequence("fixed_low", {1000, 1000, 1000}, &low)
		|| !RunSequence("fixed_high", {6000, 6000, 6000}, &high))
		return false;
	bool bPassed = dynamic[1].bitrateKbps() >= 2.0 * dynamic[0].bitrateKbps()
		&& dynamic[2].bitrateKbps() <= 0.6 * dynamic[1].bitrateKbps()
		&& dynamic[1].psnr() >= dynamic[0].psnr() && dynamic[1].psnr() >= dynamic[2].psnr();
	for (int nStage = 0; nStage < 3; ++nStage)
	{
		const auto &control = nStage == 1 ? high[nStage] : low[nStage];
		bPassed = bPassed && dynamic[nStage].nMeasuredFrames == kStageFrames - kWarmupFrames
			&& std::abs(dynamic[nStage].bitrateKbps() / control.bitrateKbps() - 1.0) <= 0.30;
	}
	return TestBitrateParameterBounds() && bPassed;
}
