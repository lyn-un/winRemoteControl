#include "codec/h264levelpolicy.h"
#include "transport/webrtc/webrtch264encoder.h"
#include "transport/webrtc/webrtch264decoder.h"

#include <api/environment/environment_factory.h>
#include <api/video/i420_buffer.h>
#include <common_video/h264/h264_common.h>
#include <media/base/sdp_video_format_utils.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <iostream>
#include <limits>

namespace
{
class KDecodedCollector final : public webrtc::DecodedImageCallback
{
public:
	int32_t Decoded(webrtc::VideoFrame &frame) override
	{
		++nFrames;
		nWidth = frame.width();
		nHeight = frame.height();
		return 0;
	}
	int nFrames = 0;
	int nWidth = 0;
	int nHeight = 0;
};

class KEncodedCollector final : public webrtc::EncodedImageCallback
{
public:
	Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *) override
	{
		images.push_back(image);
		return Result(Result::OK);
	}
	void OnFrameDropped(uint32_t, int, bool) override {}
	std::vector<webrtc::EncodedImage> images;
};
}

bool TestH264Levels()
{
	bool bPassed = true;
	const auto Check = [&bPassed](bool bValue, const char *pMessage)
	{
		if (!bValue)
		{
			std::cerr << "LEVEL FAILED: " << pMessage << '\n';
			bPassed = false;
		}
	};
	Check(KH264LevelPolicy::minimumLevel(1280, 720, 30, 6000000, 3000000, 1) == 31, "720p30");
	Check(KH264LevelPolicy::minimumLevel(1280, 720, 60, 6000000, 3000000, 1) == 32, "720p60");
	Check(KH264LevelPolicy::minimumLevel(1920, 1080, 30, 8000000, 4000000, 1) == 40, "1080p30");
	Check(KH264LevelPolicy::minimumLevel(1920, 1080, 60, 8000000, 4000000, 1) == 42, "1080p60");
	Check(KH264LevelPolicy::minimumLevel(3840, 2160, 60, 50000000, 25000000, 1) == 52, "4K60");
	Check(!KH264LevelPolicy::fits(31, 1281, 720, 30, 6000000, 0, 1), "macroblock rounding");
	Check(!KH264LevelPolicy::fits(42, 1920, 1080, 60, 50000001, 0, 1), "bitrate limit");
	Check(!KH264LevelPolicy::fits(42, 1920, 1080, 60, 8000000, 62500001, 1), "CPB limit");
	Check(!KH264LevelPolicy::fits(42, 1920, 1080, 60, 8000000, 0, 5), "DPB limit");
	Check(!KH264LevelPolicy::fits(52, (std::numeric_limits<int>::max)(),
		(std::numeric_limits<int>::max)(), (std::numeric_limits<int>::max)(), 1, 0, 1), "overflow rejected");
	Check(!KH264LevelPolicy::minimumLevel(7680, 4320, 60, 50000000, 0, 1), "unsupported 8K rejected");

	const auto format = SupportedH264Format();
	Check(NegotiatedH264Level(format) == 52, "shared capability");
	auto oldFormat = format;
	oldFormat.parameters["profile-level-id"] = "42e01f";
	oldFormat.parameters["level-asymmetry-allowed"] = "1";
	webrtc::CodecParameterMap answer;
	webrtc::H264GenerateProfileLevelIdForAnswer(format.parameters, oldFormat.parameters, &answer);
	Check(answer["profile-level-id"] == "42e01f", "old offer negotiates 3.1 despite old asymmetry=1");
	answer.clear();
	webrtc::H264GenerateProfileLevelIdForAnswer(oldFormat.parameters, format.parameters, &answer);
	Check(answer["profile-level-id"] == "42e01f", "old answer also negotiates 3.1");
	auto invalid = format;
	invalid.parameters["profile-level-id"] = "64002a";
	Check(!NegotiatedH264Level(invalid), "unsupported High profile rejected");
	invalid.parameters["profile-level-id"] = "42e03e";
	Check(!NegotiatedH264Level(invalid), "level 6.2 outside bundled WebRTC rejected");
	invalid = format;
	invalid.parameters["packetization-mode"] = "0";
	Check(!NegotiatedH264Level(invalid), "packetization mode 0 rejected");

	KWebRtcH264EncoderFactory factory(LibX264VideoEncoderPreference);
	KWebRtcH264DecoderFactory decoderFactory;
	const auto environment = webrtc::CreateEnvironment();
	const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
	webrtc::VideoCodec codec = {};
	codec.width = 1920;
	codec.height = 1080;
	codec.maxFramerate = 60;
	codec.startBitrate = 6000;
	codec.maxBitrate = 8000;
	auto oldEncoder = factory.Create(environment, oldFormat);
	Check(oldEncoder && oldEncoder->InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_ERR_PARAMETER,
		"negotiated old level reaches encoder and rejects 1080p60");

	for (const auto &size : {std::pair{1280, 720}, std::pair{1920, 1080}, std::pair{3840, 2160}})
	{
		codec.width = size.first;
		codec.height = size.second;
		auto encoder = factory.Create(environment, format);
		KEncodedCollector encoded;
		if (!encoder || encoder->InitEncode(&codec, settings) != WEBRTC_VIDEO_CODEC_OK)
		{
			Check(false, "real level encoder opens");
			continue;
		}
		encoder->RegisterEncodeCompleteCallback(&encoded);
		auto pixels = webrtc::I420Buffer::Create(size.first, size.second);
		pixels->InitializeData();
		for (int nIndex = 0; nIndex < 3; ++nIndex)
		{
			const auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(pixels)
				.set_timestamp_us(1000000 + nIndex * 1000000LL / 60).set_rtp_timestamp(nIndex * 1500).build();
			Check(encoder->Encode(frame, nullptr) == WEBRTC_VIDEO_CODEC_OK, "actual SPS passes sender guard");
		}
		Check(encoded.images.size() == 3, "all frames emitted");
		auto decoder = decoderFactory.Create(environment, format);
		KDecodedCollector decoded;
		decoder->RegisterDecodeCompleteCallback(&decoded);
		Check(decoder->Configure({}), "decoder configured");
		for (const auto &image : encoded.images)
			Check(decoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_OK, "negotiated decoder accepts actual stream");
		Check(decoded.nFrames == 3 && decoded.nWidth == size.first && decoded.nHeight == size.second,
			"real software decoder output dimensions/count");
		if (!encoded.images.empty())
		{
			const auto &image = encoded.images.front();
			QByteArray altered(reinterpret_cast<const char *>(image.data()), static_cast<qsizetype>(image.size()));
			const auto units = webrtc::H264::FindNaluIndices(std::span(image.data(), image.size()));
			for (const auto &unit : units)
			{
				if ((image.data()[unit.payload_start_offset] & 31) == 7 && unit.payload_size >= 4)
					altered[static_cast<qsizetype>(unit.payload_start_offset + 3)] = 62;
			}
			KH264SpsGuard guard;
			QString error;
			const QByteArray delta = QByteArray::fromHex("000000014101");
			Check(!guard.validate(std::span(reinterpret_cast<const uint8_t *>(delta.constData()),
				static_cast<size_t>(delta.size())), size.first, size.second, 60, &error), "delta before SPS rejected");
			Check(guard.validate(std::span(image.data(), image.size()), size.first, size.second, 60, &error),
				"later keyframe recovers after missing initial SPS");
			Check(!guard.validate(std::span(reinterpret_cast<const uint8_t *>(altered.constData()),
				static_cast<size_t>(altered.size())), size.first, size.second, 60, &error), "over-level SPS rejected");
			Check(error.contains(QStringLiteral("profileLevelId="))
				&& error.contains(QStringLiteral("spsParsed=")), "invalid SPS header has actionable diagnostics");
			Check(!guard.validate(std::span(image.data(), image.size()), size.first, size.second, 60, &error),
				"SPS failure latched");
			guard.reset();
			Check(guard.validate(std::span(image.data(), image.size()), size.first, size.second, 60, &error),
				"new session clears rejection");
			Check(!guard.validate(std::span(image.data(), image.size()), size.first + 16, size.second, 60, &error),
				"unexpected output dimensions rejected");
			if (size.first == 1920)
			{
				auto restrictedDecoder = decoderFactory.Create(environment, oldFormat);
				Check(restrictedDecoder->Configure({}), "old level decoder configured");
				Check(restrictedDecoder->Decode(image, 0) == WEBRTC_VIDEO_CODEC_ERROR,
					"receiver negotiated ceiling enforced on real SPS");
			}
		}
	}
	return bPassed;
}
