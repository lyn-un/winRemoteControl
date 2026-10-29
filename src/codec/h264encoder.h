#ifndef _WINREMOTECONTROL_H264ENCODER_H_
#define _WINREMOTECONTROL_H264ENCODER_H_

#include "core/media/videoencoderpreference.h"
#include "codec/videoencoderselection.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <memory>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
class KD3D11BgraFrame;
class KD3D11HardwareFrames;
class KGpuNv12Converter;

struct KNvencReconfiguration
{
	bool bOverride = false;
	bool bReset = true;
	bool bForceIdr = true;
};

class KH264Encoder
{
public:
	using DataCallback = std::function<void(const QByteArray &)>;
	using PacketCallback = std::function<void(const QByteArray &, std::int64_t)>;

	explicit KH264Encoder(
		KVideoEncoderPreference preference = AutoVideoEncoderPreference,
		KNvencReconfiguration nvencReconfiguration = {});
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
	// May only switch before the first submitted frame, after backend selection.
	bool enableGpuFrames(const KD3D11BgraFrame &source, QString *pErrorMessage);
	enum KGpuEncodeResult { SubmittedGpuEncodeResult, BusyGpuEncodeResult, FailedGpuEncodeResult };
	KGpuEncodeResult encodeGpuFrame(const KD3D11BgraFrame &source, qint64 nTimestampMs,
		bool bForceKeyFrame, QString *pErrorMessage);
	bool usesGpuFrames() const;
	int gpuFramePoolCapacity() const;
	int gpuFramePoolPeakInUse() const;
	void setBitrateKbps(int nBitrateKbps);
	bool setLevelLimit(int nLevel);
	void setDataCallback(const DataCallback &callback);
	void setPacketCallback(const PacketCallback &callback);
	std::int64_t lastSubmittedPts() const;
	bool close(QString *pErrorMessage);
	bool isOpen() const;

	int encodedWidth() const;
	int encodedHeight() const;
	QByteArray codecHeaderData() const;
	QString encoderName() const;
	QString fallbackReason() const;

private:
	friend class KH264EncoderTestAccess;
	bool openCodec(const char *pCodecName, bool bHardware, QString *pErrorMessage);
	bool prepareFrame(qint64 nTimestampMs, bool bForceKeyFrame, QString *pErrorMessage);
	bool writePacket(QString *pErrorMessage);
	bool submitFrame(AVFrame *pFrame, QString *pErrorMessage);
	void release();
	static QString ffmpegErrorMessage(const QString &strPrefix, int nErrorCode);

	AVCodecContext *m_pCodecContext = nullptr;
	SwsContext *m_pSwsContext = nullptr;
	AVFrame *m_pFrame = nullptr;
	AVPacket *m_pPacket = nullptr;
	DataCallback m_dataCallback;
	PacketCallback m_packetCallback;
	int m_nEncodedWidth = 0;
	int m_nEncodedHeight = 0;
	int m_nFps = 10;
	int m_nBitrateKbps = 0;
	qint64 m_nFirstTimestampMs = 0;
	std::int64_t m_nLastPts = -1;
	QString m_strEncoderName;
	QString m_strFallbackReason;
	bool m_bOpen = false;
	bool m_bReportedUnsupportedRate = false;
	int m_nLevelLimit = 0;
	KVideoEncoderPreference m_preference = AutoVideoEncoderPreference;
	KVideoEncoderCandidate m_candidate;
	KNvencReconfiguration m_nvencReconfiguration;
	std::unique_ptr<KD3D11HardwareFrames> m_upGpuFrames;
	std::unique_ptr<KGpuNv12Converter> m_upGpuConverter;
	quint64 m_nGpuGeneration = 0;
	quint64 m_nGpuAdapterLuid = 0;
};

#endif // _WINREMOTECONTROL_H264ENCODER_H_
