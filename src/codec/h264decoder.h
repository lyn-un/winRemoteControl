#ifndef _WINREMOTECONTROL_H264DECODER_H_
#define _WINREMOTECONTROL_H264DECODER_H_

#include "core/media/decodedvideoframe.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <vector>

struct AVCodecContext;
struct AVCodecParserContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class KH264Decoder
{
public:
	KH264Decoder();
	~KH264Decoder();

	KH264Decoder(const KH264Decoder &) = delete;
	KH264Decoder &operator=(const KH264Decoder &) = delete;

	bool open(QString *pErrorMessage);
	bool decode(const QByteArray &encodedData,
		quint64 nFrameIndex,
		qint64 nTimestampMs,
		std::vector<KDecodedVideoFrame> *pFrames,
		QString *pErrorMessage);
	void close();
	bool isOpen() const;

private:
	bool sendPacket(const unsigned char *pData,
		int nSize,
		quint64 nFrameIndex,
		qint64 nTimestampMs,
		std::vector<KDecodedVideoFrame> *pFrames,
		QString *pErrorMessage);
	bool receiveFrames(quint64 nFrameIndex,
		qint64 nTimestampMs,
		std::vector<KDecodedVideoFrame> *pFrames,
		QString *pErrorMessage);
	bool ensureSwsContext(const AVFrame *pFrame, QString *pErrorMessage);
	void release();
	static QString ffmpegErrorMessage(const QString &strPrefix, int nErrorCode);

	AVCodecParserContext *m_pParserContext = nullptr;
	AVCodecContext *m_pCodecContext = nullptr;
	AVFrame *m_pFrame = nullptr;
	AVPacket *m_pPacket = nullptr;
	SwsContext *m_pSwsContext = nullptr;
	int m_nSwsWidth = 0;
	int m_nSwsHeight = 0;
	int m_nSwsFormat = -1;
	bool m_bOpen = false;
};

#endif // _WINREMOTECONTROL_H264DECODER_H_
