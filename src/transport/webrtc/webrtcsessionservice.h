#ifndef _WINREMOTECONTROL_WEBRTCSESSIONSERVICE_H_
#define _WINREMOTECONTROL_WEBRTCSESSIONSERVICE_H_

#include "codec/decodedvideoframe.h"
#include "common/streamconfig.h"
#include "transport/webrtc/webrtcnetworkstats.h"
#include "transport/webrtc/webrtcpeer.h"
#include "transport/webrtc/webrtcvideoframe.h"

#include <QtCore/QObject>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

class KWebRtcSignaling;
class KInputInjector;
class QTimer;

class KWebRtcSessionService : public QObject
{
	Q_OBJECT

public:
	explicit KWebRtcSessionService(QObject *pParent = nullptr);
	~KWebRtcSessionService() override;

	KWebRtcSessionService(const KWebRtcSessionService &) = delete;
	KWebRtcSessionService &operator=(const KWebRtcSessionService &) = delete;

public slots:
	void setRole(const QString &strRole);
	void startSignalingServer(quint16 nPort);
	void connectSignaling(const QString &strHost, quint16 nPort);
	void disconnectSession();
	void enterRemoteDesktop(const KStreamConfig &config);
	void leaveRemoteDesktop();
	void startStreaming();
	void stopStreaming();
	void pushVideoFrame(const KWebRtcVideoFrame &frame);
	void sendInputMessage(const QString &strMessage);
	void sendSessionMessage(const QString &strMessage);
	void sendStreamConfig(const KStreamConfig &config);
	void handleCaptureFailure();

signals:
	void signalingChanged(const QString &strState);
	void webRtcStateChanged(const QString &strState);
	void sessionError(const QString &strMessage);
	void remoteDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);
	void remoteFrameReady(const KDecodedVideoFrame &frame);
	void remoteFrameStatsReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void networkStatsReady(const KWebRtcNetworkStats &stats);
	void startCaptureRequested();
	void stopCaptureRequested();
	void streamConfigChanged(const KStreamConfig &config);
	void inputChannelChanged(bool bOpen);
	void sessionChannelChanged(bool bOpen);
	void inputTraceUpdated(quint64 nSeq, qint64 nInjectedMs);
	void inputFeedbackFrameRequested();

private:
	enum SessionState
	{
		IdleSessionState,
		ListeningSessionState,
		ConnectingSessionState,
		NegotiatingSessionState,
		ConnectedSessionState,
		StreamingSessionState,
		InterruptedSessionState,
		StoppingSessionState
	};

	bool initializePeer(KWebRtcPeer::Role role, QString *pErrorMessage);
	void wirePeer();
	void finishSession(const QString &strReason, bool bKeepListening, bool bNotifyRemote, bool bReportError);
	void resetInputTraceState();
	void handleRemoteFrame(const KDecodedVideoFrame &frame);
	void handleInputMessage(const QString &strMessage);
	void handleInputChannelChanged(bool bOpen);
	void handleSessionChannelChanged(bool bOpen);
	void handleSessionMessage(const QString &strMessage);
	void handleInputInjected(quint64 nSeq, qint64 nInjectedMs);
	void handleOutgoingConnectionEstablished();
	void handleOutgoingConnectionFailed(const QString &strMessage);
	void handleIncomingConnectionEstablished();
	void handleSignalingConnectionLost();
	void handlePeerConnectionInterrupted();
	void handlePeerConnectionRestored();
	void handlePeerConnectionTerminated(const QString &strReason);
	void handleDisconnectGraceTimeout();
	void sendDeviceInfoMessage();
	QString createDeviceInfoMessage(const QString &strWallpaperMime, const QString &strWallpaperData) const;
	QString createControlMessage(const QString &strType) const;
	QString createStreamConfigMessage(const KStreamConfig &config) const;
	static KStreamConfig streamConfigFromJson(const QJsonObject &object);
	static QString readWallpaperBase64(QString *pMimeType);

	QString m_strRole = QStringLiteral("controller");
	bool m_bDeviceInfoRequested = false;
	bool m_bControllerConnectionPending = false;
	bool m_bEndingSession = false;
	bool m_bInputAllowed = false;
	bool m_bInputChannelOpen = false;
	bool m_bSessionChannelOpen = false;
	bool m_bStreaming = false;
	quint64 m_nLastInjectedInputSeq = 0;
	qint64 m_nLastInjectedInputMs = -1;
	SessionState m_sessionState = IdleSessionState;
	KWebRtcSignaling *m_pSignaling = nullptr;
	KWebRtcPeer *m_pPeer = nullptr;
	KInputInjector *m_pInputInjector = nullptr;
	QTimer *m_pDisconnectGraceTimer = nullptr;
};

#endif // _WINREMOTECONTROL_WEBRTCSESSIONSERVICE_H_
