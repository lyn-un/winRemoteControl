#ifndef _WINREMOTECONTROL_SESSIONVIEWMODEL_H_
#define _WINREMOTECONTROL_SESSIONVIEWMODEL_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/protocol/inputmessage.h"
#include "core/session/sessionerror.h"
#include "core/session/sessionstatemachine.h"
#include "session/inputfeedbacktracker.h"

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>

class KCaptureController;
class KSessionController;

class KSessionViewModel : public QObject
{
	Q_OBJECT

public:
	explicit KSessionViewModel(KCaptureController *pCaptureController,
		KSessionController *pSessionController,
		QObject *pParent = nullptr);
	~KSessionViewModel() override;

	KSessionViewModel(const KSessionViewModel &) = delete;
	KSessionViewModel &operator=(const KSessionViewModel &) = delete;

	QSize remoteScreenSize() const;
	KSessionState sessionState() const;

public slots:
	void startLocalPreview();
	void stopCapture();
	void setRole(const QString &strRole);
	void startSignalingServer(quint16 nPort);
	void connectSignaling(const QString &strHost, quint16 nPort);
	void retryLastConnection();
	void disconnectSession();
	void enterRemoteDesktop();
	void leaveRemoteDesktop();
	void startStreaming();
	void stopStreaming();
	void sendRemoteMouseMove(int nX, int nY);
	void sendRemoteMouseButton(int nX, int nY, int nButton, bool bPressed);
	void sendRemoteMouseWheel(int nX, int nY, int nDelta);
	void sendRemoteKey(int nVirtualKey,
		int nScanCode,
		bool bPressed,
		bool bExtended,
		bool bAutoRepeat);
	void sendRemoteText(const QString &strText);
	void sendStreamConfig(const KStreamConfig &config);
	void handleInputFeedbackRendered(quint64 nSeq);

signals:
	void statusChanged(const QString &strStatus);
	void signalingChanged(const QString &strState);
	void webRtcStateChanged(const QString &strState);
	void sessionStateChanged(KSessionState state);
	void sessionErrorOccurred(const KSessionError &error);
	void sessionChannelChanged(bool bOpen);
	void remoteDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);
	void errorOccurred(const QString &strMessage);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void renderFrameReady(const KDecodedVideoFrame &frame);
	void networkStatsReady(const KNetworkStats &stats);
	void clearPreviewRequested();
	void suspendRemoteInputRequested();

private slots:
	void handleCaptureStatusChanged(const QString &strStatus);
	void handleWebRtcStateChanged(const QString &strState);
	void handleSessionStateChanged(KSessionState state);
	void handleSessionError(const KSessionError &error);
	void handleRemoteDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);

private:
	void initConnections();
	void sendInputMessage(KInputMessage message, bool bTrace);

	KCaptureController *m_pCaptureController = nullptr;
	KSessionController *m_pSessionController = nullptr;
	quint64 m_nPointerInputSequence = 0;
	quint64 m_nReliableInputSequence = 0;
	bool m_bEnterDesktopAfterReconnect = false;
	KSessionState m_lastSessionState = IdleSessionState;
	QSize m_remoteScreenSize;
	KInputFeedbackTracker m_inputFeedbackTracker;
	KStreamConfig m_streamConfig;
};

#endif // _WINREMOTECONTROL_SESSIONVIEWMODEL_H_
