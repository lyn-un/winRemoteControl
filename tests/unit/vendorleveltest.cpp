#include "codec/h264encoder.h"
#include "transport/webrtc/webrtch264format.h"
#include "transport/webrtc/webrtch264decoder.h"

#include <api/video/encoded_image.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <iostream>
#include <vector>

namespace
{
class KLevelDecodeCollector final : public webrtc::DecodedImageCallback
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
}

bool TestHardwareLevels(KVideoEncoderPreference preference, bool bStartupRates)
{
	struct KCase { int nWidth; int nHeight; int nFps; int nLevel; };
	const KCase cases[] = {{320, 240, 30, 30}, {1280, 720, 30, 31}, {1920, 1080, 30, 40}, {1920, 1080, 60, 42},
		{1152, 720, 30, 31}, {1920, 1200, 30, 50}, {2560, 1440, 30, 50}};
	for (const auto &test : cases)
	{
		const int nFrameCount = bStartupRates ? 120 : 12;
		const int nInitialBitrateKbps = bStartupRates ? 300 : 6000;
		KH264Encoder encoder(preference);
		KH264SpsGuard guard(test.nLevel);
		KWebRtcH264Decoder decoder(test.nLevel);
		KLevelDecodeCollector collector;
		decoder.RegisterDecodeCompleteCallback(&collector);
		if (!encoder.setLevelLimit(test.nLevel) || !decoder.Configure({}))
			return false;
		std::vector<QByteArray> packets;
		QString error;
		if (!encoder.openStream(test.nWidth, test.nHeight, test.nFps, nInitialBitrateKbps,
			[&packets](const QByteArray &packet) { packets.push_back(packet); }, &error))
		{
			std::cerr << error.toStdString() << '\n';
			return false;
		}
		std::vector<unsigned char> luma(test.nWidth * test.nHeight, 64);
		std::vector<unsigned char> chroma(test.nWidth * test.nHeight / 4, 128);
		for (int nIndex = 0; nIndex < nFrameCount; ++nIndex)
		{
			if (bStartupRates && nIndex == 1)
				encoder.setBitrateKbps(235);
			if (bStartupRates && nIndex == 40)
				encoder.setBitrateKbps(6000);
			if (bStartupRates && nIndex == 80)
				encoder.setBitrateKbps(235);
			if (!encoder.encodeI420Frame(luma.data(), test.nWidth, chroma.data(), test.nWidth / 2,
				chroma.data(), test.nWidth / 2, test.nWidth, test.nHeight,
				1000 + nIndex * 1000LL / test.nFps, nIndex == 0, &error))
			{
				std::cerr << error.toStdString() << '\n';
				return false;
			}
		}
		if (!encoder.close(&error) || packets.size() != static_cast<size_t>(nFrameCount))
			return false;
		for (const auto &packet : packets)
		{
			const auto bytes = std::span(reinterpret_cast<const uint8_t *>(packet.constData()),
				static_cast<size_t>(packet.size()));
			if (!guard.validate(bytes, test.nWidth, test.nHeight, test.nFps, &error))
			{
				std::cerr << error.toStdString() << '\n';
				return false;
			}
			webrtc::EncodedImage image;
			image.SetEncodedData(webrtc::EncodedImageBuffer::Create(bytes.data(), bytes.size()));
			if (decoder.Decode(image, 0) != WEBRTC_VIDEO_CODEC_OK)
				return false;
		}
		std::cout << "levelRoundTrip width=" << test.nWidth << " height=" << test.nHeight
			<< " fps=" << test.nFps << " maximumLevel=" << test.nLevel
			<< " startupRates=" << bStartupRates
			<< " decodedFrames=" << collector.nFrames << '\n';
		if (collector.nFrames != nFrameCount || collector.nWidth != test.nWidth || collector.nHeight != test.nHeight)
			return false;
	}
	return true;
}
