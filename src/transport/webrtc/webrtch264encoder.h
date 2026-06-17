#ifndef _WINREMOTECONTROL_WEBRTCH264ENCODER_H_
#define _WINREMOTECONTROL_WEBRTCH264ENCODER_H_

#include "codec/h264encoder.h"

#include <api/video/video_frame.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_encoder.h>
#include <api/video_codecs/video_encoder_factory.h>

#include <QtCore/QByteArray>

#include <memory>
#include <vector>

class KWebRtcH264Encoder final : public webrtc::VideoEncoder
{
public:
	KWebRtcH264Encoder();
	~KWebRtcH264Encoder() override;

	int InitEncode(const webrtc::VideoCodec *pCodecSettings,
		const webrtc::VideoEncoder::Settings &settings) override;
	int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *pCallback) override;
	int32_t Release() override;
	int32_t Encode(const webrtc::VideoFrame &frame,
		const std::vector<webrtc::VideoFrameType> *pFrameTypes) override;
	void SetRates(const webrtc::VideoEncoder::RateControlParameters &parameters) override;
	webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

private:
	bool shouldForceKeyFrame(const std::vector<webrtc::VideoFrameType> *pFrameTypes) const;
	bool emitEncodedFrame(const webrtc::VideoFrame &frame,
		const QByteArray &encodedData,
		qint64 nEncodeStartMs,
		qint64 nEncodeFinishMs);
	static bool isKeyFrame(const QByteArray &encodedData);

	KH264Encoder m_encoder;
	webrtc::EncodedImageCallback *m_pCallback = nullptr;
	int m_nWidth = 0;
	int m_nHeight = 0;
	int m_nFps = 30;
	int m_nBitrateKbps = 3000;
	quint64 m_nEncodedFrameCount = 0;
	bool m_bNeedKeyFrame = true;
};

class KWebRtcH264EncoderFactory final : public webrtc::VideoEncoderFactory
{
public:
	std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
	webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
		const webrtc::SdpVideoFormat &format,
		std::optional<std::string> scalabilityMode,
		std::optional<webrtc::Resolution> resolution) const override;
	std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment &env,
		const webrtc::SdpVideoFormat &format) override;

private:
	static bool isH264Format(const webrtc::SdpVideoFormat &format);
};

#endif // _WINREMOTECONTROL_WEBRTCH264ENCODER_H_
