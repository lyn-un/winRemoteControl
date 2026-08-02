#ifndef _WINREMOTECONTROL_SESSIONCOORDINATOR_H_
#define _WINREMOTECONTROL_SESSIONCOORDINATOR_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/accessmessage.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionstatemachine.h"
#include "core/transport/remotepeertransport.h"
#include "session/sessioncontroller.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

class IKDeviceInfoProvider;
class IKInputInjector;
class KInputInjector;
class KSignalingTransport;
class QTimer;

class KSessionCoordinator : public KSessionController
{
	Q_OBJECT

public:
	explicit KSessionCoordinator(std::unique_ptr<IKDeviceInfoProvider> spDeviceInfoProvider,
		std::unique_ptr<IKInputInjector> spInputInjector,
		std::unique_ptr<KRemotePeerTransport> spRemotePeerTransport,
		std::unique_ptr<KSignalingTransport> spSignalingTransport,
		QObject *pParent = nullptr);
	~KSessionCoordinator() override;

	KSessionCoordinator(const KSessionCoordinator &) = delete;
	KSessionCoordinator &operator=(const KSessionCoordinator &) = delete;

public slots:
	void setRole(const QString &strRole) override;
	void startSignalingServer(quint16 nPort) override;
	void connectSignaling(const QString &strHost, quint16 nPort) override;
	void retryLastConnection() override;
	void disconnectSession() override;
	void enterRemoteDesktop(const KStreamConfig &config) override;
	void leaveRemoteDesktop() override;
	void startStreaming() override;
	void stopStreaming() override;
	void pushVideoFrame(const KVideoFrame &frame) override;
	void sendInputMessage(const KInputMessage &message) override;
	void sendStreamConfig(const KStreamConfig &config) override;
	void handleCaptureFailure() override;
	void applyApplicationSettings(const KApplicationSettings &settings) override;
	void respondIncomingAccessRequest(const QString &strRequestId, bool bAccepted) override;

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
	void handleIncomingConnectionEstablished(const QString &strSourceAddress, quint16 nSourcePort);
	void handleSignalingMessage(const QString &strMessage);
	void handleAccessMessage(const KAccessMessage &message);
	void handleApprovalTimeout();
	void acceptIncomingAccess();
	void rejectIncomingAccess(const QString &strReason, bool bNotifyRemote);
	void clearApprovalState(const QString &strReason);
	void sendAccessMessage(const KAccessMessage &message);
	void handleSignalingConnectionLost();
	void handlePeerConnectionInterrupted();
	void handlePeerConnectionRestored();
	void handlePeerConnectionTerminated(const QString &strReason);
	void handleReconnectTimeout();
	void sendDeviceInfoMessage();
	void updateListeningAvailability(bool bAvailable, quint16 nPort = 0);

	KSessionStateMachine m_sessionStateMachine;
	bool m_bDeviceInfoRequested = false;
	bool m_bInputChannelOpen = false;
	bool m_bSessionChannelOpen = false;
	quint64 m_nLastInjectedInputSeq = 0;
	quint64 m_nReconnectGeneration = 0;
	qint64 m_nLastInjectedInputMs = -1;
	bool m_bSignalingConnected = false;
	bool m_bListeningAvailable = false;
	quint16 m_nListeningPort = 0;
	quint16 m_nLastConnectionPort = 0;
	QString m_strLastConnectionHost;
	QElapsedTimer m_reconnectElapsedTimer;
	KApplicationSettings m_applicationSettings;
	QString m_strAccessRequestId;
	QString m_strAccessDeviceName;
	QString m_strAccessSourceAddress;
	quint64 m_nApprovalGeneration = 0;
	std::unique_ptr<IKDeviceInfoProvider> m_spDeviceInfoProvider;
	std::unique_ptr<KRemotePeerTransport> m_spRemotePeerTransport;
	std::unique_ptr<KSignalingTransport> m_spSignalingTransport;
	KSignalingTransport *m_pSignaling = nullptr;
	KInputInjector *m_pInputInjector = nullptr;
	QTimer *m_pReconnectTimer = nullptr;
	QTimer *m_pApprovalTimer = nullptr;
};

#endif // _WINREMOTECONTROL_SESSIONCOORDINATOR_H_
