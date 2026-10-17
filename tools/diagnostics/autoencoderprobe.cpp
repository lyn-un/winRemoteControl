#include "codec/h264encoder.h"
#include "common/sessiontracelogger.h"

#include <QtCore/QCoreApplication>
#include <Windows.h>
#include <iostream>
#include <set>

extern "C"
{
#include <libavcodec/avcodec.h>
}

class KH264EncoderTestAccess
{
public:
	static const AVPacket *packet(const KH264Encoder &encoder) { return encoder.m_pPacket; }
	static const AVCodecContext *context(const KH264Encoder &encoder) { return encoder.m_pCodecContext; }
};

namespace
{
bool Run(int nWidth, int nHeight, const QString &strExpected)
{
	constexpr int kFrames = 120;
	const AVCodec *pDecoder = avcodec_find_decoder(AV_CODEC_ID_H264);
	AVCodecContext *pContext = avcodec_alloc_context3(pDecoder);
	AVFrame *pFrame = av_frame_alloc();
	if (!pContext || !pFrame)
	{
		avcodec_free_context(&pContext);
		av_frame_free(&pFrame);
		return false;
	}
	pContext->thread_count = 1;
	bool bPassed = avcodec_open2(pContext, pDecoder, nullptr) >= 0;
	int nDecoded = 0;
	std::set<std::int64_t> outputPts;
	const auto decode = [&](const AVPacket *pPacket)
	{
		if (avcodec_send_packet(pContext, pPacket) < 0)
			return false;
		for (;;)
		{
			const int nResult = avcodec_receive_frame(pContext, pFrame);
			if (nResult == AVERROR(EAGAIN) || nResult == AVERROR_EOF)
				return true;
			if (nResult < 0 || pFrame->width != nWidth || pFrame->height != nHeight)
				return false;
			++nDecoded;
			av_frame_unref(pFrame);
		}
	};
	KH264Encoder encoder;
	QString strError;
	bPassed &= encoder.openStream(nWidth, nHeight, 30, 3000,
		[&](const QByteArray &)
		{
			const AVPacket *pPacket = KH264EncoderTestAccess::packet(encoder);
			bPassed &= outputPts.insert(pPacket->pts).second && decode(pPacket);
		}, &strError);
	const QString strActual = encoder.encoderName();
	if (strActual == QStringLiteral("h264_mf") || (!strExpected.isEmpty() && strActual != strExpected))
		bPassed = false;
	const AVCodecContext *pOriginal = KH264EncoderTestAccess::context(encoder);
	std::vector<unsigned char> y(nWidth * nHeight), uv(nWidth * nHeight / 4, 128);
	for (int nIndex = 0; bPassed && nIndex < kFrames; ++nIndex)
	{
		for (size_t nPixel = 0; nPixel < y.size(); ++nPixel)
			y[nPixel] = static_cast<unsigned char>(16 + (nPixel / 8 + nIndex * 7) % 220);
		encoder.setBitrateKbps(nIndex < 40 ? 1000 : nIndex < 80 ? 6000 : 1000);
		bPassed &= pOriginal == KH264EncoderTestAccess::context(encoder)
			&& encoder.encodeI420Frame(y.data(), nWidth, uv.data(), nWidth / 2, uv.data(), nWidth / 2,
				nWidth, nHeight, 1000 + nIndex * 1000LL / 30, nIndex == 0, &strError);
	}
	std::cout << "auto actual=" << strActual.toStdString() << " size=" << nWidth << 'x' << nHeight
		<< " fallback=" << encoder.fallbackReason().toStdString() << '\n';
	bPassed &= encoder.close(&strError);
	bPassed &= decode(nullptr);
	bPassed &= nDecoded == kFrames && outputPts.size() == kFrames;
	std::cout << "decoded=" << nDecoded << " uniquePts=" << outputPts.size()
		<< " passed=" << bPassed << " error=" << strError.toStdString() << std::endl;
	av_frame_free(&pFrame);
	avcodec_free_context(&pContext);
	return bPassed;
}
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hr))
		return 1;
	KSessionTraceLogger::configure(true, QCoreApplication::applicationDirPath() + QStringLiteral("/auto-encoder-logs"));
	const QString strExpected = application.arguments().value(1);
	bool bPassed = true;
	for (const auto size : {std::pair<int, int>{320, 240}, {1280, 720}, {1920, 1080}})
		bPassed &= Run(size.first, size.second, strExpected);
	CoUninitialize();
	return bPassed ? 0 : 1;
}
