#ifndef _WINREMOTECONTROL_WEBRTCPEER_H_
#define _WINREMOTECONTROL_WEBRTCPEER_H_

#include "codec/decodedvideoframe.h"
#include "common/streamconfig.h"
#include "transport/webrtc/webrtcnetworkstats.h"
#include "transport/webrtc/webrtcvideoframe.h"

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
#include <mutex>
#include <optional>
#include <vector>

class KWebRtcVideoSource;
class KCreateSessionDescriptionObserver;
namespace webrtc
{
class RTCStatsReport;
}

class KWebRtcPeer : public QObject
	, public webrtc::PeerConnectionObserver
	, public webrtc::DataChannelObserver
	, public webrtc::VideoSinkInterface<webrtc::VideoFrame>
{
	Q_OBJECT

public:
	enum Role
	{
		ControlledRole,
		ControllerRole
	};

	explicit KWebRtcPeer(QObject *pParent = nullptr);
	~KWebRtcPeer() override;

	KWebRtcPeer(const KWebRtcPeer &) = delete;
	KWebRtcPeer &operator=(const KWebRtcPeer &) = delete;

	bool initialize(Role role, QString *pErrorMessage);
	void shutdown();
	void createOffer();
	void handleSignalingMessage(const QString &strMessage);

public slots:
	void pushVideoFrame(const KWebRtcVideoFrame &frame);
	void sendInputMessage(const QString &strMessage);
	void sendSessionMessage(const QString &strMessage);
	void setStreamConfig(const KStreamConfig &config);

signals:
	void signalingMessageReady(const QString &strMessage);
	void stateChanged(const QString &strState);
	void peerError(const QString &strMessage);
	void remoteFrameReady(const KDecodedVideoFrame &frame);
	void remoteFrameStatsReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void networkStatsReady(const KWebRtcNetworkStats &stats);
	void inputMessageReceived(const QString &strMessage);
	void inputChannelChanged(bool bOpen);
	void sessionMessageReceived(const QString &strMessage);
	void sessionChannelChanged(bool bOpen);

private:
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

	void OnStateChange() override;
	void OnMessage(const webrtc::DataBuffer &buffer) override;
	void OnBufferedAmountChange(uint64_t nPreviousAmount) override;

	void OnFrame(const webrtc::VideoFrame &frame) override;
	void processLatestRemoteFrame();
	void decodeAndEmitRemoteFrame(const webrtc::VideoFrame &frame);
	void handleStatsReport(webrtc::scoped_refptr<const webrtc::RTCStatsReport> spReport);

	bool createFactory(QString *pErrorMessage);
	bool createPeerConnection(QString *pErrorMessage);
	bool createInputDataChannel(QString *pErrorMessage);
	bool createSessionDataChannel(QString *pErrorMessage);
	bool addLocalVideoTrack(QString *pErrorMessage);
	bool addRemoteVideoReceiver(QString *pErrorMessage);
	void setInputDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel);
	void setSessionDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel);
	static bool isMouseInputMessage(const QString &strMessage);
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
	void clearPendingRemoteFrame();
	void sendLatencyPing();
	void handleLatencyPing(const QJsonObject &object);
	void handleLatencyPong(const QJsonObject &object);
	static QString rtcErrorMessage(const QString &strPrefix, const webrtc::RTCError &error);

	Role m_role = ControllerRole;
	class QTimer *m_pStatsTimer = nullptr;
	std::unique_ptr<webrtc::Thread> m_spNetworkThread;
	std::unique_ptr<webrtc::Thread> m_spWorkerThread;
	std::unique_ptr<webrtc::Thread> m_spSignalingThread;
	webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_spFactory;
	webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_spPeerConnection;
	webrtc::scoped_refptr<webrtc::DataChannelInterface> m_spInputDataChannel;
	webrtc::scoped_refptr<webrtc::DataChannelInterface> m_spSessionDataChannel;
	webrtc::scoped_refptr<KWebRtcVideoSource> m_spVideoSource;
	webrtc::scoped_refptr<webrtc::RtpSenderInterface> m_spVideoSender;
	webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_spRemoteVideoTrack;
	std::mutex m_remoteFrameMutex;
	std::optional<webrtc::VideoFrame> m_pendingRemoteFrame;
	bool m_bHasPendingRemoteFrame = false;
	bool m_bRemoteFrameProcessQueued = false;
	quint64 m_nDroppedRemoteCallbackFrames = 0;
	quint64 m_nRemoteFrameIndex = 0;
	bool m_bHasPreviousStats = false;
	qint64 m_nPreviousStatsMs = 0;
	quint64 m_nPreviousBytesReceived = 0;
	qint64 m_nPreviousPacketsReceived = 0;
	qint64 m_nPreviousPacketsLost = 0;
	double m_fPreviousJitterBufferDelay = 0.0;
	double m_fPreviousJitterBufferTargetDelay = 0.0;
	quint64 m_nPreviousJitterBufferEmittedCount = 0;
	double m_fPreviousTotalDecodeTime = 0.0;
	qint64 m_nPreviousFramesDecoded = 0;
	qint64 m_nPreviousKeyFramesDecoded = 0;
	qint64 m_nPreviousFramesDropped = 0;
	quint64 m_nLatencyPingId = 0;
	int m_nDataChannelRttMs = -1;

	friend class KCreateSessionDescriptionObserver;
	friend class KStatsCallback;
};

#endif // _WINREMOTECONTROL_WEBRTCPEER_H_
