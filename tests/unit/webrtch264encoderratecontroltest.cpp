#include "transport/webrtc/webrtch264encoder.h"
#include "transport/webrtc/encoderdiagnostics.h"
#include "common/sessiontracelogger.h"
#include "common/videostagediagnostics.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <thread>

#include <api/video/video_bitrate_allocation.h>
#include <api/video/encoded_image.h>
#include <api/video/i420_buffer.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <iostream>
#include <iterator>
#include <limits>
#include <algorithm>

extern "C"
{
#include <libavcodec/avcodec.h>
}

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

	static void enqueue(KWebRtcH264Encoder *pEncoder, std::int64_t nPts, uint32_t nRtp)
	{
		KWebRtcH264Encoder::KFrameMetadata metadata;
		metadata.nRtpTimestamp = nRtp;
		metadata.nCaptureTimeMs = 1000 + nPts;
		metadata.nNtpTimeMs = 2000 + nPts;
		pEncoder->m_pendingFrames.emplace(nPts, metadata);
	}

	static bool emitPackets(KWebRtcH264Encoder *pEncoder, const std::vector<std::int64_t> &pts)
	{
		std::map<std::int64_t, QByteArray> packets;
		for (const auto nPts : pts)
			packets.emplace(nPts, QByteArray::fromHex("000000014101"));
		return pEncoder->emitPackets(packets, 3000, 1);
	}

	static size_t pendingCount(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_pendingFrames.size();
	}

	static std::int64_t lastSubmittedPts(const KWebRtcH264Encoder &encoder)
	{
		return encoder.m_encoder.lastSubmittedPts();
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
		encoder.SetRates(MakeRates(30, 0));
		Check(KWebRtcH264EncoderTestAccess::bitrateKbps(encoder) == 2500 + kRateChanges - 1,
			"zero allocation preserves the last positive bitrate");
		encoder.Release();
	}

	class KOutputCollector final : public webrtc::EncodedImageCallback
	{
	public:
		Result OnEncodedImage(const webrtc::EncodedImage &image, const webrtc::CodecSpecificInfo *) override
		{
			timestamps.push_back(image.RtpTimestamp());
			captureTimes.push_back(image.capture_time_ms_);
			packets.emplace_back(reinterpret_cast<const char *>(image.data()), image.size());
			return Result(Result::OK);
		}
		void OnFrameDropped(uint32_t, int, bool) override {}
		std::vector<uint32_t> timestamps;
		std::vector<int64_t> captureTimes;
		std::vector<QByteArray> packets;
	};

	void TestFramerateFeedbackDoesNotStopEncoding()
	{
		KWebRtcH264Encoder encoder(LibX264VideoEncoderPreference);
		KOutputCollector collector;
		encoder.RegisterEncodeCompleteCallback(&collector);
		webrtc::VideoCodec codec = {};
		codec.width = 320;
		codec.height = 240;
		codec.maxFramerate = 30;
		codec.startBitrate = 1000;
		const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
		Check(encoder.InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_OK, "feedback encoder initializes");
		auto buffer = webrtc::I420Buffer::Create(320, 240);
		buffer->InitializeData();
		const double fps[] = {30, 31, 39, 28, 13, 30, 0, -1,
			std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
			std::numeric_limits<double>::max(), 29.5};
		std::int64_t nPreviousPts = -1;
		for (int nIndex = 0; nIndex < static_cast<int>(std::size(fps)); ++nIndex)
		{
			auto rates = MakeRates(30, 1000 + nIndex * 10);
			rates.framerate_fps = fps[nIndex];
			encoder.SetRates(rates);
			Check(KWebRtcH264EncoderTestAccess::targetFps(encoder)
				== (nIndex == 3 ? 28 : nIndex == 4 ? 13 : 30), "feedback bounded to configured fps");
			Check(KWebRtcH264EncoderTestAccess::configuredFps(encoder) == 30, "configured fps unchanged");
			Check(KWebRtcH264EncoderTestAccess::bitrateKbps(encoder) == 1000 + nIndex * 10,
				"bitrate updates continue during feedback jitter");
			if (nIndex > 0)
				Check(!KWebRtcH264EncoderTestAccess::needsKeyFrame(encoder), "feedback does not request IDR");
			const auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
				.set_timestamp_us(1000000 + nIndex * 33334).build();
			Check(encoder.Encode(frame, nullptr) == WEBRTC_VIDEO_CODEC_OK, "feedback does not stop encoding");
			Check(collector.packets.size() == static_cast<size_t>(nIndex + 1), "each feedback stage outputs a frame");
			const auto nPts = KWebRtcH264EncoderTestAccess::lastSubmittedPts(encoder);
			Check(nPts > nPreviousPts, "encoder timeline is not reset by feedback");
			nPreviousPts = nPts;
		}
		encoder.Release();
		AVCodecContext *pContext = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
		AVFrame *pFrame = av_frame_alloc();
		AVPacket *pPacket = av_packet_alloc();
		if (pContext && pFrame && pPacket && avcodec_open2(pContext, pContext->codec, nullptr) >= 0)
		{
			int nDecoded = 0;
			for (const auto &packet : collector.packets)
			{
				av_packet_unref(pPacket);
				if (av_new_packet(pPacket, static_cast<int>(packet.size())) < 0)
				{
					Check(false, "allocate decode packet");
					break;
				}
				std::copy(packet.begin(), packet.end(), pPacket->data);
				Check(avcodec_send_packet(pContext, pPacket) >= 0, "continuous stream accepted by decoder");
				while (avcodec_receive_frame(pContext, pFrame) == 0)
				{
					Check(pFrame->width == 320 && pFrame->height == 240, "decoded dimensions unchanged");
					++nDecoded;
				}
			}
			Check(nDecoded == static_cast<int>(std::size(fps)), "all feedback frames continuously decode");
		}
		else
			Check(false, "feedback decoder initializes");
		av_packet_free(&pPacket);
		av_frame_free(&pFrame);
		avcodec_free_context(&pContext);
	}

	void TestDelayedPacketMetadata()
	{
		KWebRtcH264Encoder encoder(LibX264VideoEncoderPreference);
		KOutputCollector collector;
		encoder.RegisterEncodeCompleteCallback(&collector);
		webrtc::VideoCodec codec = {};
		codec.width = 320;
		codec.height = 240;
		codec.maxFramerate = 30;
		codec.startBitrate = 1000;
		const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
		Check(encoder.InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_OK, "metadata test initializes");
		auto buffer = webrtc::I420Buffer::Create(320, 240);
		buffer->InitializeData();
		const auto first = webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer)
			.set_timestamp_us(1000000).build();
		Check(encoder.Encode(first, nullptr) == WEBRTC_VIDEO_CODEC_OK, "metadata test validates real SPS first");
		collector.timestamps.clear();
		collector.captureTimes.clear();
		KWebRtcH264EncoderTestAccess::enqueue(&encoder, 0, 111);
		KWebRtcH264EncoderTestAccess::enqueue(&encoder, 1, 222);
		Check(KWebRtcH264EncoderTestAccess::emitPackets(&encoder, {}), "no output retains pending metadata");
		Check(KWebRtcH264EncoderTestAccess::pendingCount(encoder) == 2, "pending metadata retained");
		Check(KWebRtcH264EncoderTestAccess::emitPackets(&encoder, {0}), "delayed old frame emitted");
		Check(collector.timestamps == std::vector<uint32_t>{111}, "old output uses old RTP timestamp");
		Check(collector.captureTimes == std::vector<int64_t>{1000}, "old capture time preserved");
		KWebRtcH264EncoderTestAccess::enqueue(&encoder, 2, 333);
		Check(KWebRtcH264EncoderTestAccess::emitPackets(&encoder, {1, 2}), "multiple outputs emitted separately");
		Check(collector.timestamps == std::vector<uint32_t>({111, 222, 333}), "each output matched by PTS");
		Check(KWebRtcH264EncoderTestAccess::pendingCount(encoder) == 0, "consumed metadata released");
		KWebRtcH264EncoderTestAccess::enqueue(&encoder, 3, 444);
		encoder.Release();
		Check(KWebRtcH264EncoderTestAccess::pendingCount(encoder) == 0, "release clears pending metadata");
		Check(!KWebRtcH264EncoderTestAccess::emitPackets(&encoder, {3}), "late output rejected after release");
		Check(collector.timestamps.size() == 3, "late output not delivered");
	}

	void TestPendingLimit()
	{
		KWebRtcH264Encoder encoder(LibX264VideoEncoderPreference);
		KOutputCollector collector;
		webrtc::VideoCodec codec = {};
		codec.width = 320;
		codec.height = 240;
		codec.maxFramerate = 30;
		codec.startBitrate = 1000;
		const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
		Check(encoder.InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_OK, "pending limit encoder initializes");
		encoder.RegisterEncodeCompleteCallback(&collector);
		for (int nIndex = 0; nIndex < 64; ++nIndex)
			KWebRtcH264EncoderTestAccess::enqueue(&encoder, nIndex, nIndex + 1);
		const auto frame = webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(webrtc::I420Buffer::Create(320, 240)).set_timestamp_us(1000000).build();
		Check(encoder.Encode(frame, nullptr) == WEBRTC_VIDEO_CODEC_ERROR, "pending limit rejects extra input");
		Check(KWebRtcH264EncoderTestAccess::pendingCount(encoder) == 0, "pending limit clears metadata");
		Check(collector.timestamps.empty(), "overflow does not deliver mismatched output");
		encoder.Release();
	}
}

bool TestH264BitrateOutput();
bool TestH264Levels();

static void TestEncoderDiagnostics()
{
	QTemporaryDir directory;
	Check(directory.isValid(), "diagnostics temporary directory");
	KSessionTraceLogger::configure(true, directory.path());
	{
		KVideoStageDiagnostics stage(QStringLiteral("test_pipeline_watch"));
		stage.record(25, true, false, 1280, 720);
		stage.record(50, false, true);
		stage.record(75, false, false);
		KEncoderDiagnostics diagnostics;
		diagnostics.rates(QStringLiteral("requestedFps=31 configuredFps=30"));
		diagnostics.begin();
		diagnostics.phase("ffmpeg_encode");
		// No encoder progress: the independent worker must still report the phase.
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		for (int nIndex = 0; nIndex < 100; ++nIndex)
			diagnostics.error(QStringLiteral("reason=synthetic_failure"));
		diagnostics.noOutput();
		diagnostics.finish(false);
	}
	{
		KEncoderDiagnostics diagnostics;
		diagnostics.begin();
		diagnostics.phase("ffmpeg_encode");
		diagnostics.phase("process_output");
		diagnostics.output(true, 12345, true);
		diagnostics.output(false, 99999, true);
		diagnostics.finish(true);
	}
	QFile file(directory.filePath(QStringLiteral("session_trace_controlled.log")));
	Check(file.open(QIODevice::ReadOnly), "diagnostics log created");
	const QByteArray log = file.readAll();
	Check(log.contains("phase=ffmpeg_encode") && log.contains("entered=1 finished=0"),
		"watchdog reports stalled encoder independently");
	Check(log.contains("entered=1 finished=1 outputs=0 errors=1 noOutput=1 final=1"),
		"final snapshot includes error and no-output counters");
	Check(log.count("stage=encoder_error") == 1, "repeated errors are rate limited");
	Check(log.contains("requestedFps=31 configuredFps=30"), "rate details preserved");
	Check(log.contains("completed=3 success=1 skipped=1 errors=1 totalUs=150 maxUs=75 width=1280 height=720 final=1"),
		"pipeline counters retain dimensions and separate skips from errors");
	Check(log.count("stage=test_pipeline_watch") >= 2, "pipeline heartbeat and final snapshot without new frames");
	Check(log.contains("outputBytes=12345 keyFrames=1") && log.contains("measuredOutputBps="),
		"encoded byte measurement excludes rejected callback output");
	Check(log.contains("encodeTotalUs=") && log.contains("ffmpegTotalUs="), "completed phase durations reported");
	file.close();
	{
		KWebRtcH264Encoder encoder(LibX264VideoEncoderPreference);
		webrtc::VideoCodec codec = {};
		codec.width = 320;
		codec.height = 240;
		codec.maxFramerate = 30;
		codec.startBitrate = 1000;
		const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false), 1, 1200);
		Check(encoder.InitEncode(&codec, settings) == WEBRTC_VIDEO_CODEC_OK, "traced encoder initializes");
		webrtc::VideoBitrateAllocation allocation;
		allocation.SetBitrate(0, 0, 1000000);
		// A real bitrate envelope violation must still latch an error.
		allocation.SetBitrate(0, 0, 100000000);
		encoder.SetRates(webrtc::VideoEncoder::RateControlParameters(allocation, 30.0));
		const auto frame = webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(webrtc::I420Buffer::Create(320, 240)).set_timestamp_us(1000000).build();
		Check(encoder.Encode(frame, nullptr) == WEBRTC_VIDEO_CODEC_ERROR,
			"diagnostics preserves bitrate envelope rejection");
		encoder.Release();
	}
	Check(file.open(QIODevice::ReadOnly), "read encoder rejection log");
	const QByteArray rejectionLog = file.readAll();
	Check(rejectionLog.contains("reason=rate_exceeds_level")
		&& rejectionLog.contains("requestedBps=100000000"), "actual bitrate rejection records values");
	file.close();
	KSessionTraceLogger::configure(false, QString());
}

int main()
{
	TestEncoderDiagnostics();
	Check(TestH264Levels(), "H.264 level calculation, negotiation and SPS validation");
	TestRateChangesDoNotReconfigureEncoder();
	TestFramerateFeedbackDoesNotStopEncoding();
	TestDelayedPacketMetadata();
	TestPendingLimit();
	Check(TestH264BitrateOutput(), "encoded output follows dynamic bitrate");
	return g_nFailureCount == 0 ? 0 : 1;
}
