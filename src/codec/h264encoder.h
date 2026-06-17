#ifndef _WINREMOTECONTROL_H264ENCODER_H_
#define _WINREMOTECONTROL_H264ENCODER_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class KH264Encoder
{
public:
	using DataCallback = std::function<void(const QByteArray &)>;

	KH264Encoder();
	~KH264Encoder();

	KH264Encoder(const KH264Encoder &) = delete;
	KH264Encoder &operator=(const KH264Encoder &) = delete;

	bool openStream(int nWidth, int nHeight, int nFps, const DataCallback &callback, QString *pErrorMessage);
	bool openStream(int nWidth,
		int nHeight,
		int nFps,
		int nBitrateKbps,
		const DataCallback &callback,
		QString *pErrorMessage);
	bool encodeBgraFrame(const unsigned char *pBgraData,
		int nWidth,
		int nHeight,
		qint64 nTimestampMs,
		QString *pErrorMessage);
	bool encodeI420Frame(const unsigned char *pYData,
		int nStrideY,
		const unsigned char *pUData,
		int nStrideU,
		const unsigned char *pVData,
		int nStrideV,
		int nWidth,
		int nHeight,
		qint64 nTimestampMs,
		bool bForceKeyFrame,
		QString *pErrorMessage);
	void setBitrateKbps(int nBitrateKbps);
	void setDataCallback(const DataCallback &callback);
	bool close(QString *pErrorMessage);
	bool isOpen() const;

	int encodedWidth() const;
	int encodedHeight() const;

private:
	bool prepareFrame(qint64 nTimestampMs, bool bForceKeyFrame, QString *pErrorMessage);
	bool writePacket(QString *pErrorMessage);
	void release();
	static QString ffmpegErrorMessage(const QString &strPrefix, int nErrorCode);

	AVCodecContext *m_pCodecContext = nullptr;
	SwsContext *m_pSwsContext = nullptr;
	AVFrame *m_pFrame = nullptr;
	AVPacket *m_pPacket = nullptr;
	DataCallback m_dataCallback;
	int m_nEncodedWidth = 0;
	int m_nEncodedHeight = 0;
	int m_nFps = 10;
	int m_nBitrateKbps = 0;
	qint64 m_nFirstTimestampMs = 0;
	std::int64_t m_nLastPts = -1;
	bool m_bOpen = false;
};

#endif // _WINREMOTECONTROL_H264ENCODER_H_
