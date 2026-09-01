#include "transport/webrtc/webrtch264encoder.h"

#include <api/video/video_bitrate_allocation.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <iostream>
#include <iterator>

class KWebRtcH264EncoderTestAccess
{
public:
	static int configuredFps(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_nConfiguredFps;
	}

	static int targetFps(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_nTargetFps;
	}

	static int bitrateKbps(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_nBitrateKbps;
	}

	static quint64 rateTargetChangeCount(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_nRateTargetChangeCount;
	}

	static bool needsKeyFrame(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_bNeedKeyFrame;
	}

	static void clearKeyFrameRequest(KWebRtcH264Encoder *pEncoder)
	{
		pEncoder->m_bNeedKeyFrame = false;
	}
};

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const char *pDescription)
	{
		if (bCondition)
			return;
		std::cerr << "FAILED: " << pDescription << '\n';
		++g_nFailureCount;
	}

	webrtc::VideoEncoder::RateControlParameters MakeRates(int nFps, int nBitrateKbps)
	{
		webrtc::VideoBitrateAllocation allocation;
		allocation.SetBitrate(0, 0, static_cast<uint32_t>(nBitrateKbps * 1000));
		return webrtc::VideoEncoder::RateControlParameters(allocation,
			static_cast<double>(nFps));
	}

	void TestRateChangesDoNotReconfigureEncoder()
	{
		KWebRtcH264Encoder encoder(LibX264VideoEncoderPreference);
		webrtc::VideoCodec codec = {};
		codec.width = 1280;
		codec.height = 720;
		codec.maxFramerate = 60;
		codec.startBitrate = 4000;
		const webrtc::VideoEncoder::Settings settings(
			webrtc::VideoEncoder::Capabilities(false), 1, 1200);
		Check(encoder.InitEncode(&codec, settings)
			== WEBRTC_VIDEO_CODEC_OK,
			"libx264 encoder initializes for rate-control regression test");
		KWebRtcH264EncoderTestAccess::clearKeyFrameRequest(&encoder);

		constexpr int kRateChanges = 200;
		constexpr int kFpsChoices[] = { 27, 14, 15, 7, 1, 30, 60 };
		for (int nIndex = 0; nIndex < kRateChanges; ++nIndex)
		{
			encoder.SetRates(MakeRates(kFpsChoices[nIndex % std::size(kFpsChoices)],
				2500 + nIndex));
		}

		Check(KWebRtcH264EncoderTestAccess::configuredFps(encoder) == 60,
			"dynamic rate changes preserve the configured encoder frame rate");
		Check(KWebRtcH264EncoderTestAccess::targetFps(encoder)
			== kFpsChoices[(kRateChanges - 1) % std::size(kFpsChoices)],
			"latest WebRTC target frame rate is retained");
		Check(KWebRtcH264EncoderTestAccess::bitrateKbps(encoder) == 2500 + kRateChanges - 1,
			"dynamic bitrate is retained");
		Check(KWebRtcH264EncoderTestAccess::rateTargetChangeCount(encoder) > 0,
			"target frame rate changes are tracked");
		Check(!KWebRtcH264EncoderTestAccess::needsKeyFrame(encoder),
			"dynamic rate changes do not force a key frame");
		encoder.Release();
	}
}

int main()
{
	TestRateChangesDoNotReconfigureEncoder();
	return g_nFailureCount == 0 ? 0 : 1;
}
