#ifndef _WINREMOTECONTROL_SESSIONVIEWMODEL_H_
#define _WINREMOTECONTROL_SESSIONVIEWMODEL_H_

#include "codec/decodedvideoframe.h"
#include "common/streamconfig.h"
#include "transport/webrtc/webrtcnetworkstats.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

class KCaptureService;
class KWebRtcSessionService;

class KSessionViewModel : public QObject
{
	Q_OBJECT

public:
	explicit KSessionViewModel(QObject *pParent = nullptr);
	~KSessionViewModel() override;

	KSessionViewModel(const KSessionViewModel &) = delete;
	KSessionViewModel &operator=(const KSessionViewModel &) = delete;

	QSize remoteScreenSize() const;

public slots:
	void startLocalPreview();
	void stopCapture();
	void setRole(const QString &strRole);
	void startSignalingServer(quint16 nPort);
	void connectSignaling(const QString &strHost, quint16 nPort);
	void disconnectSession();
	void enterRemoteDesktop();
	void leaveRemoteDesktop();
	void startStreaming();
	void stopStreaming();
	void sendRemoteMouseMove(int nX, int nY);
	void sendRemoteMouseButton(int nX, int nY, int nButton, bool bPressed);
	void sendRemoteMouseWheel(int nX, int nY, int nDelta);
	void sendStreamConfig(const KStreamConfig &config);
	void handleInputFeedbackRendered(quint64 nSeq);

signals:
	void statusChanged(const QString &strStatus);
	void signalingChanged(const QString &strState);
	void webRtcStateChanged(const QString &strState);
	void sessionChannelChanged(bool bOpen);
	void remoteDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);
	void errorOccurred(const QString &strMessage);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void renderFrameReady(const KDecodedVideoFrame &frame);
	void networkStatsReady(const KWebRtcNetworkStats &stats);
	void clearPreviewRequested();

private slots:
	void handleCaptureStatusChanged(const QString &strStatus);
	void handleWebRtcStateChanged(const QString &strState);
	void handleRemoteDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);

private:
	void initConnections();
	void sendInputJsonMessage(QJsonObject object, bool bTrace);
	bool shouldTraceMouseMove();
	void resetInputRoundTripTrace();
	void recordInputSent(quint64 nSeq);
	void logInputRoundTripStats();

	KCaptureService *m_pCaptureService = nullptr;
	KWebRtcSessionService *m_pWebRtcSessionService = nullptr;
	quint64 m_nInputSequence = 0;
	quint64 m_nInputRoundTripSampleCount = 0;
	QSize m_remoteScreenSize;
	QElapsedTimer m_inputMoveTraceTimer;
	QElapsedTimer m_inputRoundTripTimer;
	QMap<quint64, qint64> m_inputSentTimesMs;
	QVector<qint64> m_inputRoundTripSamples;
};

#endif // _WINREMOTECONTROL_SESSIONVIEWMODEL_H_
