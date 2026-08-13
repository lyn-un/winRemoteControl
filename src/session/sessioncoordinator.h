#ifndef _WINREMOTECONTROL_SESSIONCOORDINATOR_H_
#define _WINREMOTECONTROL_SESSIONCOORDINATOR_H_

#include "core/media/decodedvideoframe.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/clipboardmessage.h"
#include "core/protocol/accessmessage.h"
#include "core/protocol/sessionmessage.h"
#include "core/protocol/protocolrouter.h"
#include "core/session/sessionstatemachine.h"
#include "core/transport/remotepeertransport.h"
#include "session/sessioncontroller.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

class IKDeviceInfoProvider;
class IKInputInjector;
class KInputInjector;
class KAccessApprovalController;
class KAccessSessionFlow;
class KCapabilitySessionFlow;
class KMediaSessionController;
class KRecoveryController;
class KSessionCommandDispatcher;
class KPeerLifecycleController;
struct KSessionCommandTransmitResult;
class KSignalingTransport;
class KShutdownCoordinator;
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
	quint64 sessionGeneration() const override;
	bool isIdle() const override;
	bool matchesCurrentEndpoint(const QString &strHost, quint16 nPort) const override;
	void setTerminalCapabilitiesAvailable(bool bControllerAvailable,
		bool bControlledAvailable);

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
	void sendClipboardMessage(const KClipboardMessage &message) override;
	bool sendTerminalControlMessage(const KTerminalMessage &message) override;
	bool sendTerminalData(const QByteArray &data) override;
	bool isTerminalBackpressured() const override;
	void sendStreamConfig(const KStreamConfig &config) override;
	void handleCaptureFailure() override;
	void applyApplicationSettings(const KApplicationSettings &settings) override;
	void respondIncomingAccessRequest(const QString &strRequestId, bool bAccepted) override;

private:
	enum PendingRequestType
	{
		NoPendingRequest,
		ListenPendingRequest,
		ConnectPendingRequest,
		RolePendingRequest
	};
	KPeerInitializationResult initializePeer(KSessionRole role);
	void handlePeerInitializationRollbackTimeout(quint64 nGeneration, int nTimeoutMs);
	void handlePeerInitializationRollbackFinished(quint64 nGeneration,
		bool bFinishedAfterTimeout);
	void wirePeer();
	void initializeProtocolRoutes();
	void initializeSessionHandlers();
	QString sendSessionMessage(KSessionMessage message);
	KSessionCommandTransmitResult transmitSessionMessage(const KSessionMessage &message);
	void handleSessionCommandCompleted(KSessionMessageType type,
		const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode,
		quint64 nGeneration);
	void handleSessionCommandTimeout(KSessionMessageType type,
		const QString &strRequestId,
		quint64 nGeneration);
	void continueStoppingTeardown();
	void finishSession(KSessionEndReason reason,
		const QString &strDetail,
		bool bKeepListening,
		bool bNotifyRemote,
		bool bReportError);
	void handleCaptureShutdownFinished(quint64 nGeneration);
	void handlePeerShutdownFinished(quint64 nGeneration);
	void finishStopping(quint64 nGeneration, bool bFinishedAfterTimeout);
	void handleStopWatchdog(quint64 nGeneration,
		bool bCapturePending,
		bool bPeerPending,
		qint64 nElapsedMs);
	void executePendingRequest();
	void resetInputTraceState();
	void handleRemoteFrame(const KDecodedVideoFrame &frame);
	void handleInputMessage(const KInputMessage &message);
	void handleInputChannelChanged(bool bOpen);
	void handleClipboardMessage(const KClipboardMessage &message);
	void handleClipboardChannelChanged(bool bOpen);
	void handleSessionChannelChanged(bool bOpen);
	void handleSessionMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleDeviceInfoRequestMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleDeviceInfoMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleStartStreamingMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleStopStreamingMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleEndSessionMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleStreamConfigMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleCapabilitiesMessage(const KSessionMessage &message);
	KProtocolHandlerResult handleCapabilityRejectedMessage(const KSessionMessage &message);
	void handleCapabilityTimeout();
	void completeCapabilityNegotiation(const KNegotiatedCapabilities &capabilities);
	KSessionCapabilities localCapabilities() const;
	void handleInputInjected(quint64 nSeq, qint64 nInjectedMs);
	void handleOutgoingConnectionEstablished();
	void handleOutgoingConnectionFailed(const QString &strMessage);
	void handleIncomingConnectionEstablished(const QString &strSourceAddress, quint16 nSourcePort);
	void handleSignalingMessage(const QString &strMessage);
	KProtocolHandlerResult handleAccessEnvelope(const KProtocolEnvelope &envelope);
	KProtocolHandlerResult handleWebRtcSignalingEnvelope(const KProtocolEnvelope &envelope);
	KProtocolHandlerResult handleBusyEnvelope(const KProtocolEnvelope &envelope);
	void handleInvalidSignalingMessage(KProtocolRouteStatus status, const QString &strError);
	void handleAccessMessage(const KAccessMessage &message);
	void handleApprovalTimeout(const QString &strRequestId, quint64 nGeneration);
	void acceptIncomingAccess();
	void rejectIncomingAccess(const QString &strReason, bool bNotifyRemote);
	void clearApprovalState(const QString &strReason);
	void sendAccessMessage(const KAccessMessage &message);
	void handleSignalingConnectionLost();
	void handlePeerConnectionInterrupted();
	void handlePeerConnectionRestored();
	void handlePeerConnectionTerminated(const QString &strReason);
	void handleReconnectTimeout(quint64 nGeneration);
	KSessionCommandTransmitResult sendDeviceInfoMessage();
	void updateListeningAvailability(bool bAvailable, quint16 nPort = 0);
	void publishSessionState();
	void reportSessionError(KSessionErrorDomain domain,
		KSessionErrorCode code,
		KSessionErrorStage stage,
		bool bRetryable,
		const QString &strTechnicalMessage);

	KSessionStateMachine m_sessionStateMachine;
	KProtocolRouter m_protocolRouter;
	KSessionCommandDispatcher *m_pSessionCommandDispatcher = nullptr;
	KRecoveryController *m_pRecoveryController = nullptr;
	KShutdownCoordinator *m_pShutdownCoordinator = nullptr;
	bool m_bDeviceInfoRequested = false;
	bool m_bInputChannelOpen = false;
	bool m_bClipboardChannelOpen = false;
	bool m_bTerminalChannelOpen = false;
	bool m_bTerminalChannelStatePublished = false;
	bool m_bControllerTerminalCapabilityAvailable = false;
	bool m_bControlledTerminalCapabilityAvailable = false;
	bool m_bSessionChannelOpen = false;
	quint64 m_nLastInjectedInputSeq = 0;
	quint64 m_nActivePeerGeneration = 0;
	qint64 m_nLastInjectedInputMs = -1;
	bool m_bListeningAvailable = false;
	quint16 m_nListeningPort = 0;
	KApplicationSettings m_applicationSettings;
	std::unique_ptr<IKDeviceInfoProvider> m_spDeviceInfoProvider;
	std::unique_ptr<KRemotePeerTransport> m_spRemotePeerTransport;
	std::unique_ptr<KSignalingTransport> m_spSignalingTransport;
	KSignalingTransport *m_pSignaling = nullptr;
	KInputInjector *m_pInputInjector = nullptr;
	KAccessApprovalController *m_pAccessApprovalController = nullptr;
	KAccessSessionFlow *m_pAccessSessionFlow = nullptr;
	KCapabilitySessionFlow *m_pCapabilitySessionFlow = nullptr;
	KMediaSessionController *m_pMediaSessionController = nullptr;
	KPeerLifecycleController *m_pPeerLifecycleController = nullptr;
	int m_nInvalidSignalingMessages = 0;
	bool m_bStopKeepListening = false;
	bool m_bStopReportError = false;
	bool m_bStopRecovering = false;
	KSessionRole m_stopRole = ControllerSessionRole;
	QString m_strStopReason;
	quint64 m_nStoppingGeneration = 0;
	QString m_strPendingEndCommandId;
	bool m_bStopTeardownStarted = false;
	PendingRequestType m_pendingRequestType = NoPendingRequest;
	QString m_strPendingHost;
	quint16 m_nPendingPort = 0;
	KSessionRole m_pendingRole = ControllerSessionRole;
};

#endif // _WINREMOTECONTROL_SESSIONCOORDINATOR_H_
