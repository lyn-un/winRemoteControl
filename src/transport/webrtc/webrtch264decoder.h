#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QtCore/QString>

#include <api/environment/environment.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_decoder.h>
#include <api/video_codecs/video_decoder_factory.h>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class KWebRtcH264Decoder : public webrtc::VideoDecoder
{
public:
	KWebRtcH264Decoder();
	~KWebRtcH264Decoder() override;

	bool Configure(const Settings &settings) override;
	int32_t Decode(const webrtc::EncodedImage &inputImage, int64_t nRenderTimeMs) override;
	int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback *pCallback) override;
	int32_t Release() override;
	DecoderInfo GetDecoderInfo() const override;

private:
	bool openDecoder();
	bool sendPacket(const unsigned char *pData, int nSize, const webrtc::EncodedImage &inputImage);
	bool receiveFrames(const webrtc::EncodedImage &inputImage);
	bool ensureSwsContext(const AVFrame *pFrame);
	void releaseDecoder();

	static QString ffmpegErrorMessage(const QString &strPrefix, int nErrorCode);

private:
	webrtc::DecodedImageCallback *m_pCallback = nullptr;
	AVCodecContext *m_pCodecContext = nullptr;
	AVFrame *m_pFrame = nullptr;
	AVPacket *m_pPacket = nullptr;
	SwsContext *m_pSwsContext = nullptr;
	int m_nSwsWidth = 0;
	int m_nSwsHeight = 0;
	int m_nSwsFormat = -1;
	quint64 m_nDecodeInputCount = 0;
	quint64 m_nDecodeOutputCount = 0;
};

class KWebRtcH264DecoderFactory : public webrtc::VideoDecoderFactory
{
public:
	std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
	CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat &format,
		bool bReferenceScaling,
		std::optional<webrtc::Resolution> resolution) const override;
	std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment &environment,
		const webrtc::SdpVideoFormat &format) override;

private:
	static bool isH264Format(const webrtc::SdpVideoFormat &format);
};
