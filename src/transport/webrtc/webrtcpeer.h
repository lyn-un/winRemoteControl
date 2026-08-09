#ifndef _WINREMOTECONTROL_WEBRTCPEER_H_
#define _WINREMOTECONTROL_WEBRTCPEER_H_

#include "core/transport/remotepeertransport.h"
#include "core/transport/networkstatstracker.h"
#include "core/transport/outboundmessagequeue.h"
#include "core/protocol/protocolrouter.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <api/media_stream_interface.h>
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <api/data_channel_interface.h>
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <pc/video_track_source.h>
#include <rtc_base/thread.h>

#include <memory>
#include <atomic>
#include <functional>
#include <vector>

class KWebRtcVideoSource;
class KCreateSessionDescriptionObserver;
class KWebRtcDataChannel;
class KWebRtcCallbackGate;
class KWebRtcLatencyProbe;
class KWebRtcRemoteFrameProcessor;
namespace webrtc
{
class RTCStatsReport;
}

class KWebRtcPeer : public KRemotePeerTransport
	, public webrtc::PeerConnectionObserver
	, public webrtc::VideoSinkInterface<webrtc::VideoFrame>
{
	Q_OBJECT

public:
	explicit KWebRtcPeer(QObject *pParent = nullptr);
	~KWebRtcPeer() override;

	KWebRtcPeer(const KWebRtcPeer &) = delete;
	KWebRtcPeer &operator=(const KWebRtcPeer &) = delete;

	KPeerInitializationResult initialize(KSessionRole role,
		quint64 nGeneration) override;
	void requestShutdown(quint64 nGeneration) override;
	quint64 generation() const override;
	void createOffer() override;
	void restartIce() override;
	void handleSignalingMessage(const KWebRtcSignalingMessage &message) override;
	void pushVideoFrame(const KVideoFrame &frame) override;
	void sendInputMessage(const KInputMessage &message) override;
	void sendClipboardMessage(const KClipboardMessage &message) override;
	KSessionMessageSendStatus sendTerminalControlMessage(
		const KTerminalMessage &message) override;
	bool sendTerminalData(const QByteArray &data) override;
	bool terminalBackpressured() const override;
	KSessionMessageSendStatus sendSessionMessage(const KSessionMessage &message) override;
	void setInputRealtimeEnabled(bool bEnabled) override;
	void setStreamConfig(const KStreamConfig &config) override;

protected:
	virtual bool shouldFailInitializationAt(KPeerInitializationStage stage) const;

private:
	enum KWebRtcPeerLifecycleState
	{
		IdleWebRtcPeerLifecycleState,
		InitializingWebRtcPeerLifecycleState,
		ReadyWebRtcPeerLifecycleState,
		FailedWebRtcPeerLifecycleState,
		ShuttingDownWebRtcPeerLifecycleState
	};

	void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
	void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
		const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>> &streams) override;
	void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
	void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
	void OnRenegotiationNeeded() override;
	void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
	void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
	void OnIceCandidate(const webrtc::IceCandidate *pCandidate) override;
	void OnIceConnectionReceivingChange(bool bReceiving) override;
	void OnIceCandidateRemoved(const webrtc::IceCandidate *pCandidate) override;
	void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> spTransceiver) override;

	void OnFrame(const webrtc::VideoFrame &frame) override;
	void handleStatsReport(webrtc::scoped_refptr<const webrtc::RTCStatsReport> spReport);

	bool createThreads(QString *pErrorMessage);
	bool createFactory(QString *pErrorMessage);
	bool createPeerConnection(QString *pErrorMessage);
	bool createInputDataChannel(QString *pErrorMessage);
	bool createRealtimeInputDataChannel(QString *pErrorMessage);
	bool createSessionDataChannel(QString *pErrorMessage);
	bool createClipboardDataChannel(QString *pErrorMessage);
	bool createTerminalDataChannel(QString *pErrorMessage);
	bool addLocalVideoTrack(QString *pErrorMessage);
	bool addRemoteVideoReceiver(QString *pErrorMessage);
	void handleInputChannelChanged(bool bOpen);
	void handleRealtimeInputChannelChanged(bool bOpen);
	void handleSessionChannelChanged(bool bOpen);
	void handleClipboardChannelChanged(bool bOpen);
	void handleTerminalChannelChanged(bool bOpen);
	void handleInputChannelMessage(const QString &strMessage);
	void handleRealtimeInputChannelMessage(const QString &strMessage);
	void handleSessionChannelMessage(const QString &strMessage);
	void handleClipboardChannelMessage(const QString &strMessage);
	KProtocolHandlerResult decodeInputMessage(const KProtocolEnvelope &envelope,
		bool bRealtimeInput);
	KProtocolHandlerResult decodeSessionMessage(const KProtocolEnvelope &envelope);
	KProtocolHandlerResult decodeClipboardMessage(const KProtocolEnvelope &envelope);
	KProtocolHandlerResult decodeTerminalMessage(const KProtocolEnvelope &envelope);
	KProtocolHandlerResult handleLatencyMessage(const KProtocolEnvelope &envelope);
	void routeDataMessage(KProtocolChannel channel,
		const QString &strMessage,
		std::atomic_int *pInvalidCount,
		bool bRealtimeInput = false);
	void handleProtocolReject(const QString &strChannel,
		int nMessageBytes,
		const QString &strError,
		std::atomic_int *pInvalidCount);
	void terminateForProtocolViolation(const QString &strChannel);
	void handleLocalDescription(webrtc::SessionDescriptionInterface *pDescription);
	void handleLocalDescriptionFailure(webrtc::RTCError error);
	void handleRemoteDescriptionSuccess(webrtc::SdpType sdpType);
	void handleRemoteDescriptionFailure(webrtc::RTCError error);
	void sendSessionDescription(const webrtc::SessionDescriptionInterface *pDescription);
	void handleSessionDescription(const QString &strType, const QString &strSdp);
	void handleIceCandidate(const QString &strSdpMid, int nSdpMLineIndex, const QString &strCandidate);
	void startStatsPolling(const QString &strReason);
	void stopStatsPolling();
	void requestStats();
	void resetStatsHistory();
	void sendLatencyPing();
	void flushInputQueue();
	void flushRealtimeInputQueue();
	void flushSessionQueue();
	void flushClipboardQueue();
	bool enqueueInputMessage(const QString &strPayload, bool bMouseMove);
	static QString rtcErrorMessage(const QString &strPrefix, const webrtc::RTCError &error);
	KPeerInitializationResult failInitialization(
		KPeerInitializationStage stage, const QString &strTechnicalMessage);
	void clearPeerObjects();
	bool postCallback(quint64 nGeneration,
		std::function<void(KWebRtcPeer *)> callback);

	KSessionRole m_role = ControllerSessionRole;
	std::atomic<quint64> m_nGeneration = 0;
	KWebRtcPeerLifecycleState m_lifecycleState = IdleWebRtcPeerLifecycleState;
	std::shared_ptr<KWebRtcCallbackGate> m_spCallbackGate;
	class QThread *m_pTeardownThread = nullptr;
	class QTimer *m_pStatsTimer = nullptr;
	std::unique_ptr<webrtc::Thread> m_spNetworkThread;
	std::unique_ptr<webrtc::Thread> m_spWorkerThread;
	std::unique_ptr<webrtc::Thread> m_spSignalingThread;
	webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_spFactory;
	webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_spPeerConnection;
	KWebRtcDataChannel *m_pInputDataChannel = nullptr;
	KWebRtcDataChannel *m_pRealtimeInputDataChannel = nullptr;
	KWebRtcDataChannel *m_pSessionDataChannel = nullptr;
	KWebRtcDataChannel *m_pClipboardDataChannel = nullptr;
	KWebRtcDataChannel *m_pTerminalDataChannel = nullptr;
	KWebRtcRemoteFrameProcessor *m_pRemoteFrameProcessor = nullptr;
	std::unique_ptr<KWebRtcLatencyProbe> m_spLatencyProbe;
	webrtc::scoped_refptr<KWebRtcVideoSource> m_spVideoSource;
	webrtc::scoped_refptr<webrtc::RtpSenderInterface> m_spVideoSender;
	webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_spRemoteVideoTrack;
	KNetworkStatsTracker m_networkStatsTracker;
	std::atomic_int m_nInvalidSignalingMessages = 0;
	std::atomic_int m_nInvalidInputMessages = 0;
	std::atomic_int m_nInvalidRealtimeInputMessages = 0;
	std::atomic_int m_nInvalidSessionMessages = 0;
	std::atomic_int m_nInvalidClipboardMessages = 0;
	std::atomic_int m_nInvalidTerminalMessages = 0;
	std::atomic_bool m_bProtocolTerminationPending = false;
	std::atomic_bool m_bInputRealtimeEnabled = false;
	KOutboundMessageQueue m_inputSendQueue { 256, 64 * 1024 };
	KOutboundMessageQueue m_realtimeInputSendQueue { 2, 4 * 1024 };
	KOutboundMessageQueue m_sessionSendQueue { 32, 512 * 1024 };
	KOutboundMessageQueue m_clipboardSendQueue { 8, 1024 * 1024 };
	KProtocolRouter m_protocolRouter;

	friend class KCreateSessionDescriptionObserver;
	friend class KStatsCallback;
};

#endif // _WINREMOTECONTROL_WEBRTCPEER_H_
