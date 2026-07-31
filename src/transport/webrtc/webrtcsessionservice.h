#ifndef _WINREMOTECONTROL_WEBRTCSESSIONSERVICE_H_
#define _WINREMOTECONTROL_WEBRTCSESSIONSERVICE_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionstatemachine.h"
#include "core/transport/remotepeertransport.h"
#include "session/sessioncontroller.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

class IKDeviceInfoProvider;
class IKInputInjector;
class KWebRtcSignaling;
class KInputInjector;
class QTimer;

class KWebRtcSessionService : public KSessionController
{
	Q_OBJECT

public:
	explicit KWebRtcSessionService(std::unique_ptr<IKDeviceInfoProvider> spDeviceInfoProvider,
		std::unique_ptr<IKInputInjector> spInputInjector,
		std::unique_ptr<KRemotePeerTransport> spRemotePeerTransport,
		QObject *pParent = nullptr);
	~KWebRtcSessionService() override;

	KWebRtcSessionService(const KWebRtcSessionService &) = delete;
	KWebRtcSessionService &operator=(const KWebRtcSessionService &) = delete;

public slots:
	void setRole(const QString &strRole) override;
	void startSignalingServer(quint16 nPort) override;
	void connectSignaling(const QString &strHost, quint16 nPort) override;
	void disconnectSession() override;
	void enterRemoteDesktop(const KStreamConfig &config) override;
	void leaveRemoteDesktop() override;
	void startStreaming() override;
	void stopStreaming() override;
	void pushVideoFrame(const KVideoFrame &frame) override;
	void sendInputMessage(const KInputMessage &message) override;
	void sendStreamConfig(const KStreamConfig &config) override;
	void handleCaptureFailure() override;

private:
	bool initializePeer(KSessionRole role, QString *pErrorMessage);
	void wirePeer();
	void sendSessionMessage(const KSessionMessage &message);
	void finishSession(KSessionEndReason reason,
		const QString &strDetail,
		bool bKeepListening,
		bool bNotifyRemote,
		bool bReportError);
	void resetInputTraceState();
	void handleRemoteFrame(const KDecodedVideoFrame &frame);
	void handleInputMessage(const KInputMessage &message);
	void handleInputChannelChanged(bool bOpen);
	void handleSessionChannelChanged(bool bOpen);
	void handleSessionMessage(const KSessionMessage &message);
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

	KSessionStateMachine m_sessionStateMachine;
	bool m_bDeviceInfoRequested = false;
	bool m_bInputChannelOpen = false;
	bool m_bSessionChannelOpen = false;
	quint64 m_nLastInjectedInputSeq = 0;
	quint64 m_nDisconnectGraceGeneration = 0;
	qint64 m_nLastInjectedInputMs = -1;
	std::unique_ptr<IKDeviceInfoProvider> m_spDeviceInfoProvider;
	std::unique_ptr<KRemotePeerTransport> m_spRemotePeerTransport;
	KWebRtcSignaling *m_pSignaling = nullptr;
	KInputInjector *m_pInputInjector = nullptr;
	QTimer *m_pDisconnectGraceTimer = nullptr;
};

#endif // _WINREMOTECONTROL_WEBRTCSESSIONSERVICE_H_
