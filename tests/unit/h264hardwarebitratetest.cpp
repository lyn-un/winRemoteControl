#include "codec/h264encoder.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/video_enc_params.h>
}

#include <QtCore/QCoreApplication>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <vector>

class KH264EncoderTestAccess
{
public:
	static const AVPacket *packet(const KH264Encoder &encoder) { return encoder.m_pPacket; }
	static const AVCodecContext *context(const KH264Encoder &encoder) { return encoder.m_pCodecContext; }
};

namespace
{
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 30;
constexpr int kStageFrames = 300;
constexpr int kWarmup = 60;

struct KResult
{
	std::uint64_t nBytes = 0;
	std::uint64_t nError = 0;
	int nFrames = 0;
	std::uint64_t nQpSum = 0;
	std::uint64_t nQpBlocks = 0;
	std::uint64_t nMaxQpBlocks = 0;
	int nMinQp = 52;
	int nMaxQp = -1;
	double kbps() const { return nBytes * 8.0 * kFps / (kStageFrames - kWarmup) / 1000; }
	double psnr() const
	{
		return nError ? 10 * std::log10(255.0 * 255 * kWidth * kHeight * nFrames / nError) : 100;
	}
};

class KReferenceDecoder
{
public:
	KReferenceDecoder()
	{
		m_texture.resize(kWidth * kHeight);
		std::uint32_t nSeed = 0x6d2b79f5;
		for (auto &value : m_texture)
		{
			nSeed ^= nSeed << 13;
			nSeed ^= nSeed >> 17;
			nSeed ^= nSeed << 5;
			value = static_cast<unsigned char>(16 + nSeed % 220);
		}
	}
	~KReferenceDecoder()
	{
		av_frame_free(&m_pFrame);
		avcodec_free_context(&m_pContext);
	}
	KReferenceDecoder(const KReferenceDecoder &) = delete;
	KReferenceDecoder &operator=(const KReferenceDecoder &) = delete;
	bool open()
	{
		m_pContext = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
		m_pFrame = av_frame_alloc();
		if (!m_pContext || !m_pFrame)
			return false;
		m_pContext->thread_count = 1;
		m_pContext->export_side_data |= AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
		return avcodec_open2(m_pContext, nullptr, nullptr) >= 0;
	}
	unsigned char pixel(int nFrame, int x, int y) const
	{
		const int nLocal = nFrame % kStageFrames;
		return m_texture[((y + nLocal * 3) % kHeight) * kWidth + (x + nLocal * 7) % kWidth];
	}
	bool submit(const AVPacket *pPacket)
	{
		if (pPacket)
		{
			if (pPacket->pts < 0 || pPacket->pts >= kStageFrames * 3)
				return false;
			const int nFrame = static_cast<int>(pPacket->pts);
			bool bIdr = false;
			for (int nIndex = 0; nIndex + 4 < pPacket->size; ++nIndex)
			{
				if (pPacket->data[nIndex] || pPacket->data[nIndex + 1])
					continue;
				int nHeader = -1;
				if (pPacket->data[nIndex + 2] == 1)
					nHeader = nIndex + 3;
				else if (!pPacket->data[nIndex + 2] && pPacket->data[nIndex + 3] == 1)
					nHeader = nIndex + 4;
				if (nHeader >= 0 && (pPacket->data[nHeader] & 31) == 5)
					bIdr = true;
			}
			if (bIdr)
				keyFrames.push_back(nFrame);
			if (nFrame % kStageFrames >= kWarmup)
				results[nFrame / kStageFrames].nBytes += pPacket->size;
		}
		if (avcodec_send_packet(m_pContext, pPacket) < 0)
			return false;
		int nResult;
		while ((nResult = avcodec_receive_frame(m_pContext, m_pFrame)) >= 0)
		{
			const auto nPts = m_pFrame->pts;
			if (nPts < 0 || nPts >= kStageFrames * 3 || !m_seen.insert(nPts).second
				|| m_pFrame->width != kWidth || m_pFrame->height != kHeight
				|| m_pFrame->format != AV_PIX_FMT_YUV420P)
				return false;
			const int nFrame = static_cast<int>(nPts);
			if (nFrame % kStageFrames >= kWarmup)
			{
				auto &result = results[nFrame / kStageFrames];
				++result.nFrames;
				const AVFrameSideData *pSideData = av_frame_get_side_data(m_pFrame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
				if (pSideData && pSideData->size >= sizeof(AVVideoEncParams))
				{
					auto *pParameters = reinterpret_cast<AVVideoEncParams *>(pSideData->data);
					if (pParameters->type != AV_VIDEO_ENC_PARAMS_H264)
						return false;
					for (unsigned int nBlock = 0; nBlock < pParameters->nb_blocks; ++nBlock)
					{
						const int nQp = pParameters->qp + av_video_enc_params_block(pParameters, nBlock)->delta_qp;
						if (nQp < 0 || nQp > 51)
							return false;
						result.nQpSum += nQp;
						++result.nQpBlocks;
						result.nMaxQpBlocks += nQp == 51;
						result.nMinQp = std::min(result.nMinQp, nQp);
						result.nMaxQp = std::max(result.nMaxQp, nQp);
					}
				}
				for (int y = 0; y < kHeight; ++y)
					for (int x = 0; x < kWidth; ++x)
					{
						const int nDifference = pixel(nFrame, x, y) - m_pFrame->data[0][y * m_pFrame->linesize[0] + x];
						result.nError += nDifference * nDifference;
					}
			}
			av_frame_unref(m_pFrame);
		}
		return nResult == AVERROR(EAGAIN) || nResult == AVERROR_EOF;
	}
	bool complete() const { return m_seen.size() == kStageFrames * 3; }
	int profile() const { return m_pContext->profile; }
	int level() const { return m_pContext->level; }
	std::array<KResult, 3> results;
	std::vector<int> keyFrames;
private:
	AVCodecContext *m_pContext = nullptr;
	AVFrame *m_pFrame = nullptr;
	std::vector<unsigned char> m_texture;
	std::set<std::int64_t> m_seen;
};

bool Run(KVideoEncoderPreference preference, const char *pName, std::array<int, 3> rates, std::array<KResult, 3> *pResults)
{
	KReferenceDecoder decoder;
	bool bDecoded = decoder.open();
	KH264Encoder encoder(preference);
	QString strError;
	if (!bDecoded || !encoder.openStream(kWidth, kHeight, kFps, rates[0],
		[&](const QByteArray &) { bDecoded = decoder.submit(KH264EncoderTestAccess::packet(encoder)) && bDecoded; },
		&strError))
	{
		std::cerr << "Hardware encoder initialization failed: " << strError.toStdString() << '\n';
		return false;
	}
	const auto *pOriginal = KH264EncoderTestAccess::context(encoder);
	std::vector<unsigned char> luma(kWidth * kHeight), chroma(kWidth * kHeight / 4, 128);
	for (int nFrame = 0; nFrame < kStageFrames * 3; ++nFrame)
	{
		if (nFrame % kStageFrames == 0)
			encoder.setBitrateKbps(rates[nFrame / kStageFrames]);
		if (KH264EncoderTestAccess::context(encoder) != pOriginal)
			return false;
		for (int y = 0; y < kHeight; ++y)
			for (int x = 0; x < kWidth; ++x)
				luma[y * kWidth + x] = decoder.pixel(nFrame, x, y);
		if (!encoder.encodeI420Frame(luma.data(), kWidth, chroma.data(), kWidth / 2,
			chroma.data(), kWidth / 2, kWidth, kHeight, 1000 + nFrame * 1000LL / kFps, nFrame == 0, &strError)
			|| !bDecoded)
		{
			std::cerr << pName << " frame=" << nFrame << " error=" << strError.toStdString() << '\n';
			return false;
		}
	}
	if (!encoder.close(&strError) || !bDecoded || !decoder.submit(nullptr) || !decoder.complete())
		return false;
	std::cout << pName << " decodedFrames=900 rebuilds=0 profile=" << decoder.profile()
		<< " level=" << decoder.level() << " idrPts=";
	for (const int nPts : decoder.keyFrames)
		std::cout << nPts << ',';
	std::cout << std::endl;
	if (decoder.profile() != AV_PROFILE_H264_BASELINE && decoder.profile() != AV_PROFILE_H264_CONSTRAINED_BASELINE)
		return false;
	*pResults = decoder.results;
	for (int nStage = 0; nStage < 3; ++nStage)
	{
		const auto &result = (*pResults)[nStage];
		std::cout << pName << " stage=" << nStage << " requestedKbps=" << rates[nStage]
			<< " measuredKbps=" << result.kbps() << " lumaPsnrDb=" << result.psnr()
			<< " measuredFrames=" << result.nFrames
			<< " targetDeviationPercent=" << (result.kbps() / rates[nStage] - 1) * 100
			<< " qpBlocks=" << result.nQpBlocks
			<< " qpMin=" << (result.nQpBlocks ? result.nMinQp : -1) << " qpMax=" << result.nMaxQp
			<< " qpMean=" << (result.nQpBlocks ? static_cast<double>(result.nQpSum) / result.nQpBlocks : -1)
			<< " qp51Percent=" << (result.nQpBlocks ? 100.0 * result.nMaxQpBlocks / result.nQpBlocks : -1)
			<< std::endl;
	}
	return true;
}

bool TestRepeatedRateChanges(KVideoEncoderPreference preference)
{
	KH264Encoder encoder(preference);
	QString strError;
	std::set<std::int64_t> outputPts;
	if (!encoder.openStream(320, 240, 30, 1000, [](const QByteArray &) {}, &strError))
		return false;
	encoder.setPacketCallback([&](const QByteArray &, std::int64_t nPts) { outputPts.insert(nPts); });
	const auto *pOriginal = KH264EncoderTestAccess::context(encoder);
	const int nInitialBufferBits = pOriginal->rc_buffer_size;
	std::vector<unsigned char> y(320 * 240, 96), uv(160 * 120, 128);
	for (int nIndex = 0; nIndex < 200; ++nIndex)
	{
		const int nKbps = 1000 + nIndex * 5;
		encoder.setBitrateKbps(nKbps);
		encoder.setBitrateKbps(nKbps);
		encoder.setBitrateKbps(0);
		encoder.setBitrateKbps(-1);
		if (nIndex == 0)
			encoder.setBitrateKbps((std::numeric_limits<int>::max)());
		const int nExpectedBufferBits = preference == QuickSyncVideoEncoderPreference
			? nInitialBufferBits : nKbps * 250;
		if (KH264EncoderTestAccess::context(encoder) != pOriginal || pOriginal->bit_rate != nKbps * 1000LL
			|| pOriginal->rc_max_rate != nKbps * 1000LL || pOriginal->rc_buffer_size != nExpectedBufferBits
			|| !encoder.encodeI420Frame(y.data(), 320, uv.data(), 160, uv.data(), 160,
				320, 240, 1000 + nIndex * 1000LL / 30, nIndex == 0, &strError))
		{
			std::cerr << "Repeated rate update failed: " << strError.toStdString() << '\n';
			return false;
		}
	}
	if (!encoder.close(&strError) || outputPts.size() != 200)
		return false;
	std::cout << "rateRegression updates=200 rebuilds=0 inputKeyframeRequests=1 outputFrames=200" << std::endl;
	return true;
}
}

bool TestHardwareBitrateOutput(KVideoEncoderPreference preference)
{
	std::cout << "FFmpeg=" << av_version_info() << std::endl;
	if (preference != MediaFoundationVideoEncoderPreference && !TestRepeatedRateChanges(preference))
		return false;
	std::array<KResult, 3> dynamic, low, high;
	if (!Run(preference, "dynamic", {1000, 6000, 1000}, &dynamic)
		|| !Run(preference, "fixed_low", {1000, 1000, 1000}, &low)
		|| !Run(preference, "fixed_high", {6000, 6000, 6000}, &high))
		return false;
	bool bPassed = dynamic[1].kbps() >= 2 * dynamic[0].kbps()
		&& dynamic[2].kbps() <= .6 * dynamic[1].kbps()
		&& dynamic[1].psnr() >= dynamic[0].psnr() && dynamic[1].psnr() >= dynamic[2].psnr();
	for (int nStage = 0; nStage < 3; ++nStage)
	{
		const auto &control = nStage == 1 ? high[nStage] : low[nStage];
		bPassed = bPassed && dynamic[nStage].nFrames == kStageFrames - kWarmup
			&& low[nStage].nFrames == kStageFrames - kWarmup && high[nStage].nFrames == kStageFrames - kWarmup
			&& control.kbps() > 0 && std::abs(dynamic[nStage].kbps() / control.kbps() - 1) <= .3;
	}
	return bPassed;
}
