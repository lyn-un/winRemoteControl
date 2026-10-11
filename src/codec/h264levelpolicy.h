#ifndef _WINREMOTECONTROL_H264LEVELPOLICY_H_
#define _WINREMOTECONTROL_H264LEVELPOLICY_H_

#include <cstdint>

// Progressive, 8-bit Constrained Baseline policy. No WebRTC/FFmpeg types.
class KH264LevelPolicy
{
public:
	static constexpr int kMaximumLevel = 52;
	static bool isSupported(int nLevel);
	static std::int64_t maximumBitrate(int nLevel);
	static bool fits(int nLevel, int nWidth, int nHeight, int nFps,
		std::int64_t nBitrateBps, std::int64_t nBufferBits, int nReferenceFrames);
	static int minimumLevel(int nWidth, int nHeight, int nFps,
		std::int64_t nBitrateBps, std::int64_t nBufferBits, int nReferenceFrames);
};

#endif // _WINREMOTECONTROL_H264LEVELPOLICY_H_
