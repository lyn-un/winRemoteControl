#ifndef _WINREMOTECONTROL_SESSIONVIEWMODEL_H_
#define _WINREMOTECONTROL_SESSIONVIEWMODEL_H_

#include "codec/decodedvideoframe.h"
#include "common/streamconfig.h"
#include "transport/webrtc/webrtcnetworkstats.h"

#include <QtCore/QObject>
#include <QtCore/QString>

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

private:
	void initConnections();

	KCaptureService *m_pCaptureService = nullptr;
	KWebRtcSessionService *m_pWebRtcSessionService = nullptr;
};

#endif // _WINREMOTECONTROL_SESSIONVIEWMODEL_H_
