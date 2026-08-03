#ifndef _WINREMOTECONTROL_SESSIONCONTROLLER_H_
#define _WINREMOTECONTROL_SESSIONCONTROLLER_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/clipboardmessage.h"
#include "core/settings/applicationsettings.h"
#include "core/session/sessionerror.h"
#include "core/session/sessionstatemachine.h"

#include <QtCore/QObject>
#include <QtCore/QString>

class KSessionController : public QObject
{
	Q_OBJECT

public:
	explicit KSessionController(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KSessionController() override = default;

	KSessionController(const KSessionController &) = delete;
	KSessionController &operator=(const KSessionController &) = delete;

public slots:
	virtual void setRole(const QString &strRole) = 0;
	virtual void startSignalingServer(quint16 nPort) = 0;
	virtual void connectSignaling(const QString &strHost, quint16 nPort) = 0;
	virtual void retryLastConnection() = 0;
	virtual void disconnectSession() = 0;
	virtual void enterRemoteDesktop(const KStreamConfig &config) = 0;
	virtual void leaveRemoteDesktop() = 0;
	virtual void startStreaming() = 0;
	virtual void stopStreaming() = 0;
	virtual void pushVideoFrame(const KVideoFrame &frame) = 0;
	virtual void sendInputMessage(const KInputMessage &message) = 0;
	virtual void sendClipboardMessage(const KClipboardMessage &message) = 0;
	virtual void sendStreamConfig(const KStreamConfig &config) = 0;
	virtual void handleCaptureFailure() = 0;
	virtual void applyApplicationSettings(const KApplicationSettings &settings) = 0;
	virtual void respondIncomingAccessRequest(const QString &strRequestId, bool bAccepted) = 0;

signals:
	void listeningAvailabilityChanged(bool bAvailable, quint16 nPort);
	void signalingChanged(const QString &strState);
	void webRtcStateChanged(const QString &strState);
	void sessionStateChanged(KSessionState state);
	void sessionErrorOccurred(const KSessionError &error);
	void remoteDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);
	void remoteFrameReady(const KDecodedVideoFrame &frame);
	void remoteFrameStatsReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void networkStatsReady(const KNetworkStats &stats);
	void startCaptureRequested();
	void stopCaptureRequested();
	void streamConfigChanged(const KStreamConfig &config);
	void inputChannelChanged(bool bOpen);
	void clipboardMessageReceived(const KClipboardMessage &message);
	void clipboardChannelChanged(bool bOpen);
	void sessionChannelChanged(bool bOpen);
	void inputTraceUpdated(quint64 nSeq, qint64 nInjectedMs);
	void inputFeedbackFrameRequested();
	void incomingAccessObserved(const QString &strDeviceName, const QString &strSourceAddress);
	void incomingAccessRequest(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strSourceAddress,
		qint64 nExpiresAtMs);
	void incomingAccessRequestCleared(const QString &strRequestId, const QString &strReason);
};

#endif // _WINREMOTECONTROL_SESSIONCONTROLLER_H_
