#include "codec/h264levelpolicy.h"

#include <algorithm>
#include <iterator>

namespace
{
struct KLevelLimit
{
	int nLevel;
	int nMacroblocksPerSecond;
	int nFrameMacroblocks;
	int nDpbMacroblocks;
	int nBitrateKbps;
	int nCpbKbits;
};

// ITU-T H.264 Annex A, Table A-1; Baseline VCL bitrate/CPB factors.
// 1b is deliberately not advertised or accepted by this implementation.
constexpr KLevelLimit kLimits[] = {
	{10, 1485, 99, 396, 64, 175}, {11, 3000, 396, 900, 192, 500},
	{12, 6000, 396, 2376, 384, 1000}, {13, 11880, 396, 2376, 768, 2000},
	{20, 11880, 396, 2376, 2000, 2000}, {21, 19800, 792, 4752, 4000, 4000},
	{22, 20250, 1620, 8100, 4000, 4000}, {30, 40500, 1620, 8100, 10000, 10000},
	{31, 108000, 3600, 18000, 14000, 14000}, {32, 216000, 5120, 20480, 20000, 20000},
	{40, 245760, 8192, 32768, 20000, 25000}, {41, 245760, 8192, 32768, 50000, 62500},
	{42, 522240, 8704, 34816, 50000, 62500}, {50, 589824, 22080, 110400, 135000, 135000},
	{51, 983040, 36864, 184320, 240000, 240000}, {52, 2073600, 36864, 184320, 240000, 240000}
};

const KLevelLimit *FindLimit(int nLevel)
{
	const auto *pLimit = std::find_if(std::begin(kLimits), std::end(kLimits),
		[nLevel](const KLevelLimit &limit) { return limit.nLevel == nLevel; });
	return pLimit == std::end(kLimits) ? nullptr : pLimit;
}
}

bool KH264LevelPolicy::isSupported(int nLevel)
{
	return FindLimit(nLevel) != nullptr;
}

std::int64_t KH264LevelPolicy::maximumBitrate(int nLevel)
{
	const auto *pLimit = FindLimit(nLevel);
	return pLimit ? static_cast<std::int64_t>(pLimit->nBitrateKbps) * 1000 : 0;
}

bool KH264LevelPolicy::fits(int nLevel, int nWidth, int nHeight, int nFps,
	std::int64_t nBitrateBps, std::int64_t nBufferBits, int nReferenceFrames)
{
	const auto *pLimit = FindLimit(nLevel);
	if (!pLimit || nWidth <= 0 || nHeight <= 0 || nFps <= 0
		|| nBitrateBps <= 0 || nBufferBits < 0 || nReferenceFrames < 0 || nReferenceFrames > 16)
		return false;
	const std::int64_t nColumns = (static_cast<std::int64_t>(nWidth) + 15) / 16;
	const std::int64_t nRows = (static_cast<std::int64_t>(nHeight) + 15) / 16;
	const std::int64_t nBlocks = nColumns * nRows;
	// Check size first so multiplication by FPS cannot overflow.
	return nBlocks <= pLimit->nFrameMacroblocks
		&& nColumns * nColumns <= 8LL * pLimit->nFrameMacroblocks
		&& nRows * nRows <= 8LL * pLimit->nFrameMacroblocks
		&& nBlocks * nFps <= pLimit->nMacroblocksPerSecond
		&& nBlocks * std::max(1, nReferenceFrames) <= pLimit->nDpbMacroblocks
		&& nBitrateBps <= maximumBitrate(nLevel)
		&& nBufferBits <= static_cast<std::int64_t>(pLimit->nCpbKbits) * 1000;
}

int KH264LevelPolicy::minimumLevel(int nWidth, int nHeight, int nFps,
	std::int64_t nBitrateBps, std::int64_t nBufferBits, int nReferenceFrames)
{
	for (const auto &limit : kLimits)
	{
		if (fits(limit.nLevel, nWidth, nHeight, nFps, nBitrateBps, nBufferBits, nReferenceFrames))
			return limit.nLevel;
	}
	return 0;
}
