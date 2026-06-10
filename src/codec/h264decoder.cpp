#include "codec/h264decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <utility>

KH264Decoder::KH264Decoder()
{
}

KH264Decoder::~KH264Decoder()
{
	close();
}

bool KH264Decoder::open(QString *pErrorMessage)
{
	if (isOpen())
		close();

	const AVCodec *pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (pCodec == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("FFmpeg H.264 decoder not found");
		return false;
	}

	m_pParserContext = av_parser_init(AV_CODEC_ID_H264);
	if (m_pParserContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create H.264 parser failed");
		return false;
	}

	m_pCodecContext = avcodec_alloc_context3(pCodec);
	if (m_pCodecContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create H.264 decoder context failed");
		release();
		return false;
	}

	m_pCodecContext->thread_count = 1;

	const int nOpenResult = avcodec_open2(m_pCodecContext, pCodec, nullptr);
	if (nOpenResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Open H.264 decoder failed"), nOpenResult);
		release();
		return false;
	}

	m_pFrame = av_frame_alloc();
	m_pPacket = av_packet_alloc();
	if (m_pFrame == nullptr || m_pPacket == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create H.264 decoder buffers failed");
		release();
		return false;
	}

	m_bOpen = true;
	return true;
}

bool KH264Decoder::decode(const QByteArray &encodedData,
	quint64 nFrameIndex,
	qint64 nTimestampMs,
	std::vector<KDecodedVideoFrame> *pFrames,
	QString *pErrorMessage)
{
	if (!isOpen())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("H.264 decoder is not open");
		return false;
	}

	if (pFrames == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Decoded frame output is null");
		return false;
	}

	const unsigned char *pInputData = reinterpret_cast<const unsigned char *>(encodedData.constData());
	int nInputSize = encodedData.size();

	while (nInputSize > 0)
	{
		unsigned char *pPacketData = nullptr;
		int nPacketSize = 0;
		const int nParsedSize = av_parser_parse2(m_pParserContext,
			m_pCodecContext,
			&pPacketData,
			&nPacketSize,
			pInputData,
			nInputSize,
			AV_NOPTS_VALUE,
			AV_NOPTS_VALUE,
			0);
		if (nParsedSize < 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Parse H.264 packet failed");
			return false;
		}

		pInputData += nParsedSize;
		nInputSize -= nParsedSize;

		if (nPacketSize > 0)
		{
			if (!sendPacket(pPacketData, nPacketSize, nFrameIndex, nTimestampMs, pFrames, pErrorMessage))
				return false;
		}
	}

	return true;
}

void KH264Decoder::close()
{
	release();
}

bool KH264Decoder::isOpen() const
{
	return m_bOpen && m_pCodecContext != nullptr && m_pParserContext != nullptr;
}

bool KH264Decoder::sendPacket(const unsigned char *pData,
	int nSize,
	quint64 nFrameIndex,
	qint64 nTimestampMs,
	std::vector<KDecodedVideoFrame> *pFrames,
	QString *pErrorMessage)
{
	av_packet_unref(m_pPacket);
	const int nPacketResult = av_new_packet(m_pPacket, nSize);
	if (nPacketResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Allocate H.264 packet failed"), nPacketResult);
		return false;
	}

	std::memcpy(m_pPacket->data, pData, static_cast<size_t>(nSize));

	const int nSendResult = avcodec_send_packet(m_pCodecContext, m_pPacket);
	av_packet_unref(m_pPacket);
	if (nSendResult < 0)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Send packet to H.264 decoder failed"), nSendResult);
		return false;
	}

	return receiveFrames(nFrameIndex, nTimestampMs, pFrames, pErrorMessage);
}

bool KH264Decoder::receiveFrames(quint64 nFrameIndex,
	qint64 nTimestampMs,
	std::vector<KDecodedVideoFrame> *pFrames,
	QString *pErrorMessage)
{
	while (true)
	{
		const int nReceiveResult = avcodec_receive_frame(m_pCodecContext, m_pFrame);
		if (nReceiveResult == AVERROR(EAGAIN) || nReceiveResult == AVERROR_EOF)
			return true;
		if (nReceiveResult < 0)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = ffmpegErrorMessage(QStringLiteral("Receive decoded frame failed"), nReceiveResult);
			return false;
		}

		if (!ensureSwsContext(m_pFrame, pErrorMessage))
		{
			av_frame_unref(m_pFrame);
			return false;
		}

		KDecodedVideoFrame decodedFrame;
		decodedFrame.nWidth = m_pFrame->width;
		decodedFrame.nHeight = m_pFrame->height;
		decodedFrame.nFrameIndex = nFrameIndex;
		decodedFrame.nTimestampMs = nTimestampMs;
		decodedFrame.vecBgraBuffer.resize(static_cast<size_t>(decodedFrame.nWidth) * decodedFrame.nHeight * 4);

		unsigned char *pDstData[] = { decodedFrame.vecBgraBuffer.data(), nullptr, nullptr, nullptr };
		const int nDstStride[] = { decodedFrame.nWidth * 4, 0, 0, 0 };
		const int nScaleResult = sws_scale(m_pSwsContext,
			m_pFrame->data,
			m_pFrame->linesize,
			0,
			m_pFrame->height,
			pDstData,
			nDstStride);
		av_frame_unref(m_pFrame);

		if (nScaleResult != decodedFrame.nHeight)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Convert decoded frame to BGRA failed");
			return false;
		}

		pFrames->push_back(std::move(decodedFrame));
	}
}

bool KH264Decoder::ensureSwsContext(const AVFrame *pFrame, QString *pErrorMessage)
{
	const int nFormat = static_cast<int>(pFrame->format);
	if (m_pSwsContext != nullptr &&
		m_nSwsWidth == pFrame->width &&
		m_nSwsHeight == pFrame->height &&
		m_nSwsFormat == nFormat)
	{
		return true;
	}

	if (m_pSwsContext != nullptr)
	{
		sws_freeContext(m_pSwsContext);
		m_pSwsContext = nullptr;
	}

	m_pSwsContext = sws_getContext(pFrame->width,
		pFrame->height,
		static_cast<AVPixelFormat>(pFrame->format),
		pFrame->width,
		pFrame->height,
		AV_PIX_FMT_BGRA,
		SWS_FAST_BILINEAR,
		nullptr,
		nullptr,
		nullptr);
	if (m_pSwsContext == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create decoded frame converter failed");
		return false;
	}

	m_nSwsWidth = pFrame->width;
	m_nSwsHeight = pFrame->height;
	m_nSwsFormat = nFormat;
	return true;
}

void KH264Decoder::release()
{
	if (m_pSwsContext != nullptr)
	{
		sws_freeContext(m_pSwsContext);
		m_pSwsContext = nullptr;
	}

	if (m_pPacket != nullptr)
	{
		av_packet_free(&m_pPacket);
		m_pPacket = nullptr;
	}

	if (m_pFrame != nullptr)
	{
		av_frame_free(&m_pFrame);
		m_pFrame = nullptr;
	}

	if (m_pCodecContext != nullptr)
	{
		avcodec_free_context(&m_pCodecContext);
		m_pCodecContext = nullptr;
	}

	if (m_pParserContext != nullptr)
	{
		av_parser_close(m_pParserContext);
		m_pParserContext = nullptr;
	}

	m_nSwsWidth = 0;
	m_nSwsHeight = 0;
	m_nSwsFormat = -1;
	m_bOpen = false;
}

QString KH264Decoder::ffmpegErrorMessage(const QString &strPrefix, int nErrorCode)
{
	char szBuffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(nErrorCode, szBuffer, sizeof(szBuffer));
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromLocal8Bit(szBuffer));
}
