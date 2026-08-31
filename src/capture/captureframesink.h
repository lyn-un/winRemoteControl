#ifndef _WINREMOTECONTROL_CAPTUREFRAMESINK_H_
#define _WINREMOTECONTROL_CAPTUREFRAMESINK_H_

#include "core/media/captureframesink.h"
#include "core/media/decodedvideoframe.h"
#include "core/media/videoframe.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>

#include <memory>
#include <mutex>
#include <vector>

class KH264Decoder;
class KH264Encoder;
struct SwsContext;

class KCaptureFrameSink : public QObject, public IKCaptureFrameSink
{
	Q_OBJECT

public:
	enum SinkMode
	{
		LocalPreviewSinkMode,
		RemoteVideoSinkMode
	};

	explicit KCaptureFrameSink(SinkMode mode, QObject *pParent = nullptr);
	~KCaptureFrameSink() override;

	bool initialize(QString *pErrorMessage) override;
	bool processFrame(KCaptureFrame &frame, QString *pErrorMessage) override;
	void handleCaptureTimeout() override;
	void shutdown() override;
	void setStreamConfig(const KStreamConfig &config) override;
	void setInputTraceState(quint64 nSeq, qint64 nInjectedMs) override;

	KStreamConfig streamConfig() const;

signals:
	void decodedFrameReady(const KDecodedVideoFrame &frame);
	void videoFrameReady(const KVideoFrame &frame);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);

private:
	enum FrameConversionResult
	{
		ConvertedFrameConversionResult,
		DroppedFrameConversionResult,
		FailedFrameConversionResult
	};

	void inputTraceState(quint64 *pSeq, qint64 *pInjectedMs) const;
	bool processRemoteFrame(KCaptureFrame &frame,
		const KStreamConfig &config,
		QString *pErrorMessage);
	bool processLocalPreviewFrame(KCaptureFrame &frame, QString *pErrorMessage);
	FrameConversionResult convertBgraToI420(KCaptureFrame &captureFrame,
		const KStreamConfig &config,
		quint64 nLastInputSeq,
		qint64 nLastInputAgeMs,
		KVideoFrame *pVideoFrame);
	std::shared_ptr<KI420FrameBuffer> acquireFrameBuffer(int nWidth, int nHeight);
	static void resizeFrameBuffer(KI420FrameBuffer *pBuffer, int nWidth, int nHeight);
	qint64 framePoolBytes() const;
	static KStreamConfig normalizeStreamConfig(const KStreamConfig &config);

	SinkMode m_mode = LocalPreviewSinkMode;
	mutable std::mutex m_configMutex;
	mutable std::mutex m_inputTraceMutex;
	KStreamConfig m_streamConfig;
	quint64 m_nLastInputSeq = 0;
	qint64 m_nLastInputInjectedMs = -1;
	std::unique_ptr<KH264Encoder> m_upEncoder;
	std::unique_ptr<KH264Decoder> m_upDecoder;
	bool m_bCodecOpen = false;
	bool m_bDecodeOk = true;
	QElapsedTimer m_initialFrameRetryTimer;
	KVideoFrame m_lastVideoFrame;
	int m_nInitialFrameRetryCount = 0;
	SwsContext *m_pSwsContext = nullptr;
	std::vector<std::shared_ptr<KI420FrameBuffer>> m_vecFrameBufferPool;
	quint64 m_nFramePoolDrops = 0;
};

#endif // _WINREMOTECONTROL_CAPTUREFRAMESINK_H_
