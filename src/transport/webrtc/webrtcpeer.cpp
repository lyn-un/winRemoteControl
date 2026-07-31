#include "transport/webrtc/webrtcpeer.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "transport/webrtc/webrtcdatachannel.h"
#include "transport/webrtc/webrtch264decoder.h"
#include "transport/webrtc/webrtch264encoder.h"
#include "transport/webrtc/webrtclatencyprobe.h"
#include "transport/webrtc/webrtcremoteframeprocessor.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_modular_peer_connection_factory.h>
#include <api/enable_media.h>
#include <api/environment/environment_factory.h>
#include <api/jsep.h>
#include <api/make_ref_counted.h>
#include <api/media_types.h>
#include <api/rtp_receiver_interface.h>
#include <api/stats/rtc_stats_collector_callback.h>
#include <api/stats/rtc_stats_report.h>
#include <api/stats/rtcstats_objects.h>
#include <api/rtp_transceiver_direction.h>
#include <api/rtp_transceiver_interface.h>
#include <api/video/video_source_interface.h>
#include <api/video/i420_buffer.h>
#include <pc/video_track_source.h>
#include <rtc_base/logging.h>

#include <libyuv.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <optional>

class KStatsCallback : public webrtc::RTCStatsCollectorCallback
{
public:
	explicit KStatsCallback(KWebRtcPeer *pPeer)
		: m_pPeer(pPeer)
	{
	}

	void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &spReport) override
	{
		const QPointer<KWebRtcPeer> pPeer = m_pPeer;
		if (!pPeer)
			return;

		QMetaObject::invokeMethod(pPeer,
			[pPeer, spReport]()
			{
				if (pPeer)
					pPeer->handleStatsReport(spReport);
			},
			Qt::QueuedConnection);
	}

private:
	QPointer<KWebRtcPeer> m_pPeer;
};

namespace
{
	constexpr char kMessageType[] = "messageType";
	constexpr char kSdpType[] = "sdpType";
	constexpr char kSdp[] = "sdp";
	constexpr char kSdpMid[] = "sdpMid";
	constexpr char kSdpMLineIndex[] = "sdpMLineIndex";
	constexpr char kCandidate[] = "candidate";
	constexpr char kOffer[] = "offer";
	constexpr char kAnswer[] = "answer";
	constexpr char kIceCandidate[] = "iceCandidate";
	constexpr char kStreamId[] = "wrc-stream";
	constexpr char kVideoLabel[] = "wrc-screen";
	constexpr char kInputChannelLabel[] = "input";
	constexpr char kSessionChannelLabel[] = "session";
	constexpr int kStatsPollingIntervalMs = 1000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr int kReceiverMaxFrameRateFps = 60;
	constexpr double kSecondsToMilliseconds = 1000.0;
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr char kPlayoutDelayUri[] = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";

	static int secondsToMs(double fSeconds)
	{
		return static_cast<int>(std::round(fSeconds * kSecondsToMilliseconds));
	}

	static QString stringViewToQString(absl::string_view value)
	{
		return QString::fromLatin1(value.data(), static_cast<int>(value.size()));
	}

	static QString roleToString(KSessionRole role)
	{
		return role == ControllerSessionRole
			? QStringLiteral("controller")
			: QStringLiteral("controlled");
	}

	class KWebRtcRawVideoSource : public webrtc::VideoSourceInterface<webrtc::VideoFrame>
	{
	public:
		void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *pSink,
			const webrtc::VideoSinkWants &wants) override
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			for (SinkItem &item : m_vecSinks)
			{
				if (item.pSink == pSink)
				{
					item.wants = wants;
					return;
				}
			}
			m_vecSinks.push_back({ pSink, wants });
		}

		void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *pSink) override
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			m_vecSinks.erase(std::remove_if(m_vecSinks.begin(),
					m_vecSinks.end(),
					[pSink](const SinkItem &item)
					{
						return item.pSink == pSink;
					}),
				m_vecSinks.end());
		}

		void pushFrame(const KVideoFrame &frame)
		{
			if (frame.nWidth <= 0 || frame.nHeight <= 0)
				return;

			webrtc::scoped_refptr<webrtc::I420Buffer> spBuffer =
				webrtc::I420Buffer::Create(frame.nWidth, frame.nHeight);
			copyPlane(reinterpret_cast<const unsigned char *>(frame.yPlane.constData()),
				frame.nStrideY,
				spBuffer->MutableDataY(),
				spBuffer->StrideY(),
				frame.nWidth,
				frame.nHeight);
			copyPlane(reinterpret_cast<const unsigned char *>(frame.uPlane.constData()),
				frame.nStrideU,
				spBuffer->MutableDataU(),
				spBuffer->StrideU(),
				frame.nWidth / 2,
				frame.nHeight / 2);
			copyPlane(reinterpret_cast<const unsigned char *>(frame.vPlane.constData()),
				frame.nStrideV,
				spBuffer->MutableDataV(),
				spBuffer->StrideV(),
				frame.nWidth / 2,
				frame.nHeight / 2);

			const webrtc::VideoFrame videoFrame = webrtc::VideoFrame::Builder()
				.set_video_frame_buffer(spBuffer)
				.set_timestamp_ms(frame.nTimestampMs)
				.set_rotation(webrtc::kVideoRotation_0)
				.build();

			std::vector<webrtc::VideoSinkInterface<webrtc::VideoFrame> *> vecSinks;
			{
				std::lock_guard<std::mutex> guard(m_mutex);
				for (const SinkItem &item : m_vecSinks)
					vecSinks.push_back(item.pSink);
			}

			for (webrtc::VideoSinkInterface<webrtc::VideoFrame> *pSink : vecSinks)
			{
				if (pSink != nullptr)
					pSink->OnFrame(videoFrame);
			}
		}

	private:
		struct SinkItem
		{
			webrtc::VideoSinkInterface<webrtc::VideoFrame> *pSink = nullptr;
			webrtc::VideoSinkWants wants;
		};

		static void copyPlane(const unsigned char *pSrc,
			int nSrcStride,
			unsigned char *pDst,
			int nDstStride,
			int nWidth,
			int nHeight)
		{
			if (pSrc == nullptr || pDst == nullptr)
				return;

			// libyuv::CopyPlane uses SIMD memcpy and handles stride alignment,
			// replacing the per-row std::memcpy loop.
			libyuv::CopyPlane(pSrc,
				nSrcStride,
				pDst,
				nDstStride,
				nWidth,
				nHeight);
		}

		std::mutex m_mutex;
		std::vector<SinkItem> m_vecSinks;
	};

	class KSetDescriptionObserver : public webrtc::SetSessionDescriptionObserver
	{
	public:
		using SuccessCallback = std::function<void()>;
		using FailureCallback = std::function<void(webrtc::RTCError)>;

		static webrtc::scoped_refptr<KSetDescriptionObserver> create(SuccessCallback successCallback = {},
			FailureCallback failureCallback = {})
		{
			return webrtc::make_ref_counted<KSetDescriptionObserver>(std::move(successCallback),
				std::move(failureCallback));
		}

		KSetDescriptionObserver(SuccessCallback successCallback, FailureCallback failureCallback)
			: m_successCallback(std::move(successCallback))
			, m_failureCallback(std::move(failureCallback))
		{
		}

		void OnSuccess() override
		{
			if (m_successCallback)
				m_successCallback();
		}

		void OnFailure(webrtc::RTCError error) override
		{
			if (m_failureCallback)
				m_failureCallback(error);
			RTC_LOG(LS_WARNING) << error.message();
		}

	private:
		SuccessCallback m_successCallback;
		FailureCallback m_failureCallback;
	};
}

class KCreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver
{
public:
	static webrtc::scoped_refptr<KCreateSessionDescriptionObserver> create(KWebRtcPeer *pPeer)
	{
		return webrtc::make_ref_counted<KCreateSessionDescriptionObserver>(pPeer);
	}

	explicit KCreateSessionDescriptionObserver(KWebRtcPeer *pPeer)
		: m_pPeer(pPeer)
	{
	}

	void OnSuccess(webrtc::SessionDescriptionInterface *pDescription) override
	{
		if (m_pPeer != nullptr)
			m_pPeer->handleLocalDescription(pDescription);
	}

	void OnFailure(webrtc::RTCError error) override
	{
		if (m_pPeer != nullptr)
			m_pPeer->handleLocalDescriptionFailure(error);
	}

private:
	KWebRtcPeer *m_pPeer = nullptr;
};

class KWebRtcVideoSource : public webrtc::VideoTrackSource
{
public:
	KWebRtcVideoSource()
		: VideoTrackSource(false)
	{
		SetState(kLive);
	}

	void pushFrame(const KVideoFrame &frame)
	{
		m_source.pushFrame(frame);
	}

private:
	webrtc::VideoSourceInterface<webrtc::VideoFrame> *source() override
	{
		return &m_source;
	}

	KWebRtcRawVideoSource m_source;
};

KWebRtcPeer::KWebRtcPeer(QObject *pParent)
	: KRemotePeerTransport(pParent)
	, m_pInputDataChannel(new KWebRtcDataChannel(this))
	, m_pSessionDataChannel(new KWebRtcDataChannel(this))
	, m_pRemoteFrameProcessor(new KWebRtcRemoteFrameProcessor(this))
	, m_spLatencyProbe(std::make_unique<KWebRtcLatencyProbe>())
{
	connect(m_pInputDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleInputChannelChanged);
	connect(m_pInputDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, &KWebRtcPeer::handleInputChannelMessage);
	connect(m_pSessionDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleSessionChannelChanged);
	connect(m_pSessionDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, &KWebRtcPeer::handleSessionChannelMessage);
	connect(m_pRemoteFrameProcessor, &KWebRtcRemoteFrameProcessor::frameReady,
		this, &KWebRtcPeer::remoteFrameReady);
	connect(m_pRemoteFrameProcessor, &KWebRtcRemoteFrameProcessor::frameStatsReady,
		this, &KWebRtcPeer::remoteFrameStatsReady);
}

KWebRtcPeer::~KWebRtcPeer()
{
	shutdown();
}

bool KWebRtcPeer::initialize(KSessionRole role, QString *pErrorMessage)
{
	shutdown();
	m_role = role;
	if (!createFactory(pErrorMessage))
		return false;
	if (!createPeerConnection(pErrorMessage))
		return false;
	if (m_role == ControllerSessionRole && !createInputDataChannel(pErrorMessage))
		return false;
	if (m_role == ControllerSessionRole && !createSessionDataChannel(pErrorMessage))
		return false;
	if (m_role == ControlledSessionRole && !addLocalVideoTrack(pErrorMessage))
		return false;
	if (m_role == ControllerSessionRole && !addRemoteVideoReceiver(pErrorMessage))
		return false;

	emit stateChanged(QStringLiteral("PeerReady"));
	return true;
}

void KWebRtcPeer::shutdown()
{
	stopStatsPolling();
	m_pInputDataChannel->clear();
	m_pSessionDataChannel->clear();

	if (m_spRemoteVideoTrack)
		m_spRemoteVideoTrack->RemoveSink(this);
	m_spRemoteVideoTrack = nullptr;
	m_pRemoteFrameProcessor->clear();
	m_spVideoSender = nullptr;
	m_spPeerConnection = nullptr;
	m_spVideoSource = nullptr;
	m_spFactory = nullptr;

	if (m_spSignalingThread)
		m_spSignalingThread->Stop();
	if (m_spWorkerThread)
		m_spWorkerThread->Stop();
	if (m_spNetworkThread)
		m_spNetworkThread->Stop();
	m_spSignalingThread.reset();
	m_spWorkerThread.reset();
	m_spNetworkThread.reset();
	resetStatsHistory();
}

void KWebRtcPeer::createOffer()
{
	if (!m_spPeerConnection)
		return;
	m_spPeerConnection->CreateOffer(KCreateSessionDescriptionObserver::create(this).get(),
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void KWebRtcPeer::handleSignalingMessage(const QString &strMessage)
{
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return;

	const QJsonObject object = document.object();
	const QString strMessageType = object.value(QString::fromLatin1(kMessageType)).toString();
	if (strMessageType == QString::fromLatin1(kOffer) || strMessageType == QString::fromLatin1(kAnswer))
	{
		handleSessionDescription(strMessageType, object.value(QString::fromLatin1(kSdp)).toString());
	}
	else if (strMessageType == QString::fromLatin1(kIceCandidate))
	{
		handleIceCandidate(object.value(QString::fromLatin1(kSdpMid)).toString(),
			object.value(QString::fromLatin1(kSdpMLineIndex)).toInt(),
			object.value(QString::fromLatin1(kCandidate)).toString());
	}
}

void KWebRtcPeer::pushVideoFrame(const KVideoFrame &frame)
{
	if (frame.nFrameIndex > 0 && frame.nFrameIndex % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(roleToString(m_role),
			QStringLiteral("webrtc_push"),
			QStringLiteral("frame=%1 width=%2 height=%3 timestampMs=%4")
				.arg(frame.nFrameIndex)
				.arg(frame.nWidth)
				.arg(frame.nHeight)
				.arg(frame.nTimestampMs));
	}

	if (m_spVideoSource)
		m_spVideoSource->pushFrame(frame);
}

void KWebRtcPeer::sendInputMessage(const KInputMessage &message)
{
	m_pInputDataChannel->sendText(KInputMessageCodec::encode(message));
}

void KWebRtcPeer::sendLatencyPing()
{
	if (m_role != ControllerSessionRole
		|| !m_pInputDataChannel->isOpen())
	{
		return;
	}
	m_pInputDataChannel->sendText(m_spLatencyProbe->createPing());
}

void KWebRtcPeer::sendSessionMessage(const KSessionMessage &message)
{
	m_pSessionDataChannel->sendText(KSessionMessageCodec::encode(message));
}

void KWebRtcPeer::setStreamConfig(const KStreamConfig &config)
{
	if (!m_spVideoSender)
		return;

	webrtc::RtpParameters parameters = m_spVideoSender->GetParameters();
	if (parameters.encodings.empty())
		parameters.encodings.emplace_back();

	for (webrtc::RtpEncodingParameters &encoding : parameters.encodings)
	{
		encoding.max_bitrate_bps = config.nBitrateKbps * kBitsPerKilobit;
		encoding.max_framerate = static_cast<double>(config.nFps);
	}

	const webrtc::RTCError result = m_spVideoSender->SetParameters(parameters);
	if (!result.ok())
	{
		emit transportError(rtcErrorMessage(QStringLiteral("Set video stream parameters failed"), result));
		return;
	}

	KLatencyTraceLogger::write(roleToString(m_role),
		QStringLiteral("stream_config_applied"),
		QStringLiteral("fps=%1 width=%2 height=%3 bitrateKbps=%4")
			.arg(config.nFps)
			.arg(config.nWidth)
			.arg(config.nHeight)
			.arg(config.nBitrateKbps));
}

void KWebRtcPeer::startStatsPolling(const QString &strReason)
{
	if (QThread::currentThread() != thread())
	{
		QMetaObject::invokeMethod(this,
			[this, strReason]()
			{
				startStatsPolling(strReason);
			},
			Qt::QueuedConnection);
		return;
	}

	if (m_role != ControllerSessionRole)
		return;

	if (m_pStatsTimer == nullptr)
	{
		m_pStatsTimer = new QTimer(this);
		connect(m_pStatsTimer, &QTimer::timeout,
			this, &KWebRtcPeer::requestStats);
	}

	if (!m_pStatsTimer->isActive())
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("stats_polling_start"),
			QStringLiteral("reason=%1").arg(strReason));
		m_pStatsTimer->start(kStatsPollingIntervalMs);
	}
	requestStats();
}

void KWebRtcPeer::stopStatsPolling()
{
	if (QThread::currentThread() != thread())
	{
		QMetaObject::invokeMethod(this,
			[this]()
			{
				stopStatsPolling();
			},
			Qt::QueuedConnection);
		return;
	}

	if (m_pStatsTimer != nullptr)
		m_pStatsTimer->stop();

	resetStatsHistory();
	KNetworkStats stats;
	emit networkStatsReady(stats);
}

void KWebRtcPeer::requestStats()
{
	if (m_role != ControllerSessionRole || !m_spPeerConnection || !m_spSignalingThread)
		return;

	sendLatencyPing();

	webrtc::scoped_refptr<KStatsCallback> spCallback =
		webrtc::make_ref_counted<KStatsCallback>(this);
	webrtc::scoped_refptr<webrtc::PeerConnectionInterface> spPeerConnection = m_spPeerConnection;
	m_spSignalingThread->PostTask(
		[spPeerConnection, spCallback]()
		{
			if (spPeerConnection)
				spPeerConnection->GetStats(spCallback.get());
		});
}

void KWebRtcPeer::resetStatsHistory()
{
	m_networkStatsTracker.reset();
	m_spLatencyProbe->reset();
}

void KWebRtcPeer::handleStatsReport(webrtc::scoped_refptr<const webrtc::RTCStatsReport> spReport)
{
	if (!spReport || m_role != ControllerSessionRole)
		return;

	KNetworkStatsSample sample;
	sample.nTimestampMs = spReport->timestamp().ms();

	for (const webrtc::RTCInboundRtpStreamStats *pInbound :
		spReport->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>())
	{
		if (pInbound->kind && *pInbound->kind != "video")
			continue;

		sample.bHasInboundVideo = true;
		if (pInbound->bytes_received)
			sample.nBytesReceived = *pInbound->bytes_received;
		if (pInbound->packets_received)
			sample.nPacketsReceived = *pInbound->packets_received;
		if (pInbound->packets_lost)
			sample.nPacketsLost = *pInbound->packets_lost;
		if (pInbound->jitter)
			sample.nJitterMs = secondsToMs(*pInbound->jitter);
		if (pInbound->frames_per_second)
			sample.nFps = static_cast<int>(std::round(*pInbound->frames_per_second));
		if (pInbound->jitter_buffer_delay)
			sample.fJitterBufferDelaySeconds = *pInbound->jitter_buffer_delay;
		if (pInbound->jitter_buffer_target_delay)
			sample.fJitterBufferTargetDelaySeconds = *pInbound->jitter_buffer_target_delay;
		if (pInbound->jitter_buffer_emitted_count)
			sample.nJitterBufferEmittedCount = *pInbound->jitter_buffer_emitted_count;
		if (pInbound->total_decode_time)
			sample.fTotalDecodeTimeSeconds = *pInbound->total_decode_time;
		if (pInbound->frames_decoded)
			sample.nFramesDecoded = *pInbound->frames_decoded;
		if (pInbound->key_frames_decoded)
			sample.nKeyFramesDecoded = *pInbound->key_frames_decoded;
		if (pInbound->frames_dropped)
			sample.nFramesDropped = *pInbound->frames_dropped;
		break;
	}

	for (const webrtc::RTCIceCandidatePairStats *pCandidatePair :
		spReport->GetStatsOfType<webrtc::RTCIceCandidatePairStats>())
	{
		const bool bSucceeded = pCandidatePair->state && *pCandidatePair->state == "succeeded";
		const bool bNominated = pCandidatePair->nominated && *pCandidatePair->nominated;
		if (!bSucceeded || !bNominated || !pCandidatePair->current_round_trip_time)
			continue;

		sample.nRttMs = secondsToMs(*pCandidatePair->current_round_trip_time);
		break;
	}

	const KNetworkStats stats = m_networkStatsTracker.update(
		sample,
		m_spLatencyProbe->roundTripMs());
	if (sample.bHasInboundVideo)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("video_stats"),
			QStringLiteral("jitterBufferMs=%1 targetMs=%2 decodeMs=%3 framesDecoded=%4 keyFramesDecoded=%5 dropped=%6")
				.arg(stats.nJitterBufferDelayMs)
				.arg(stats.nJitterBufferTargetDelayMs)
				.arg(stats.nDecodeTimeMs)
				.arg(stats.nFramesDecoded)
				.arg(stats.nKeyFramesDecoded)
				.arg(stats.nFramesDropped));
	}
	emit networkStatsReady(stats);
}

void KWebRtcPeer::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState)
{
}

void KWebRtcPeer::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
	const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>> &)
{
	if (!receiver || !receiver->track()
		|| receiver->track()->kind() != webrtc::MediaStreamTrackInterface::kVideoKind)
	{
		return;
	}

	m_spRemoteVideoTrack = static_cast<webrtc::VideoTrackInterface *>(receiver->track().get());
	webrtc::VideoSinkWants wants;
	wants.black_frames = false;
	wants.max_framerate_fps = kReceiverMaxFrameRateFps;
	m_spRemoteVideoTrack->AddOrUpdateSink(this, wants);
	emit stateChanged(QStringLiteral("RemoteVideoTrack"));
	startStatsPolling(QStringLiteral("remote_video_track"));
}

void KWebRtcPeer::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>)
{
	if (m_spRemoteVideoTrack)
		m_spRemoteVideoTrack->RemoveSink(this);
	m_spRemoteVideoTrack = nullptr;
	m_pRemoteFrameProcessor->clear();
}

void KWebRtcPeer::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
{
	if (!channel)
		return;
	if (channel->label() == kInputChannelLabel)
		m_pInputDataChannel->setChannel(channel);
	else if (channel->label() == kSessionChannelLabel)
		m_pSessionDataChannel->setChannel(channel);
}

void KWebRtcPeer::OnRenegotiationNeeded()
{
}

void KWebRtcPeer::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state)
{
	emit stateChanged(stringViewToQString(webrtc::PeerConnectionInterface::AsString(new_state)));
	if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted)
	{
		if (m_role == ControllerSessionRole)
			startStatsPolling(QStringLiteral("ice_connected"));
		emit connectionRestored();
	}
	else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected)
	{
		stopStatsPolling();
		emit connectionInterrupted();
	}
	else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionClosed)
	{
		stopStatsPolling();
		emit connectionTerminated(
			new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed
				? QStringLiteral("ice_failed")
				: QStringLiteral("ice_closed"));
	}
}

void KWebRtcPeer::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState)
{
}

void KWebRtcPeer::OnIceCandidate(const webrtc::IceCandidate *pCandidate)
{
	if (pCandidate == nullptr)
		return;

	QJsonObject object;
	object.insert(QString::fromLatin1(kMessageType), QString::fromLatin1(kIceCandidate));
	object.insert(QString::fromLatin1(kSdpMid), QString::fromStdString(pCandidate->sdp_mid()));
	object.insert(QString::fromLatin1(kSdpMLineIndex), pCandidate->sdp_mline_index());
	object.insert(QString::fromLatin1(kCandidate), QString::fromStdString(pCandidate->ToString()));
	emit signalingMessageReady(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebRtcPeer::OnIceConnectionReceivingChange(bool)
{
}

void KWebRtcPeer::OnIceCandidateRemoved(const webrtc::IceCandidate *)
{
}

void KWebRtcPeer::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> spTransceiver)
{
	if (!spTransceiver || !spTransceiver->receiver())
		return;
	OnAddTrack(spTransceiver->receiver(), spTransceiver->receiver()->streams());
}

void KWebRtcPeer::handleInputChannelChanged(bool bOpen)
{
	emit inputChannelChanged(bOpen);
	if (bOpen)
		startStatsPolling(QStringLiteral("input_channel_open"));
}

void KWebRtcPeer::handleSessionChannelChanged(bool bOpen)
{
	emit sessionChannelChanged(bOpen);
	emit stateChanged(bOpen
		? QStringLiteral("SessionChannelOpen")
		: QStringLiteral("SessionChannelClosed"));
}

void KWebRtcPeer::handleSessionChannelMessage(const QString &strMessage)
{
	KSessionMessage message;
	QString strError;
	if (!KSessionMessageCodec::decode(strMessage, &message, &strError))
	{
		KSessionTraceLogger::write(roleToString(m_role),
			QStringLiteral("protocol_reject"),
			QStringLiteral("session"),
			strMessage.toUtf8().size(),
			strError);
		return;
	}
	emit sessionMessageReceived(message);
}

void KWebRtcPeer::handleInputChannelMessage(const QString &strMessage)
{
	QString strResponse;
	if (m_spLatencyProbe->handleMessage(strMessage, m_role, &strResponse))
	{
		if (!strResponse.isEmpty() && m_pInputDataChannel->isOpen())
			m_pInputDataChannel->sendText(strResponse);
		return;
	}

	KInputMessage message;
	QString strError;
	if (!KInputMessageCodec::decode(strMessage, &message, &strError))
	{
		KSessionTraceLogger::write(roleToString(m_role),
			QStringLiteral("protocol_reject"),
			QStringLiteral("input"),
			strMessage.toUtf8().size(),
			strError);
		return;
	}
	emit inputMessageReceived(message);
}

void KWebRtcPeer::handleLocalDescription(webrtc::SessionDescriptionInterface *pDescription)
{
	if (m_spPeerConnection == nullptr || pDescription == nullptr)
		return;

	sendSessionDescription(pDescription);
	m_spPeerConnection->SetLocalDescription(KSetDescriptionObserver::create(
			[this]()
			{
				emit stateChanged(QStringLiteral("LocalDescriptionSet"));
			},
			[this](webrtc::RTCError error)
			{
				handleLocalDescriptionFailure(error);
			})
			.get(),
		pDescription);
}

void KWebRtcPeer::handleLocalDescriptionFailure(webrtc::RTCError error)
{
	emit transportError(rtcErrorMessage(QStringLiteral("Create SDP failed"), error));
}

void KWebRtcPeer::handleRemoteDescriptionSuccess(webrtc::SdpType sdpType)
{
	emit stateChanged(QStringLiteral("RemoteDescriptionSet"));
	if (sdpType == webrtc::SdpType::kOffer && m_spPeerConnection)
	{
		m_spPeerConnection->CreateAnswer(KCreateSessionDescriptionObserver::create(this).get(),
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
}

void KWebRtcPeer::handleRemoteDescriptionFailure(webrtc::RTCError error)
{
	emit transportError(rtcErrorMessage(QStringLiteral("Set remote SDP failed"), error));
}

void KWebRtcPeer::OnFrame(const webrtc::VideoFrame &frame)
{
	m_pRemoteFrameProcessor->enqueue(frame);
}

bool KWebRtcPeer::createFactory(QString *pErrorMessage)
{
	m_spNetworkThread = webrtc::Thread::CreateWithSocketServer();
	m_spWorkerThread = webrtc::Thread::Create();
	m_spSignalingThread = webrtc::Thread::Create();
	if (!m_spNetworkThread || !m_spWorkerThread || !m_spSignalingThread)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create WebRTC threads failed");
		return false;
	}

	m_spNetworkThread->Start();
	m_spWorkerThread->Start();
	m_spSignalingThread->Start();

	webrtc::PeerConnectionFactoryDependencies deps;
	deps.network_thread = m_spNetworkThread.get();
	deps.worker_thread = m_spWorkerThread.get();
	deps.signaling_thread = m_spSignalingThread.get();
	deps.env = webrtc::CreateEnvironment();
	deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
	deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
	deps.video_encoder_factory = std::make_unique<KWebRtcH264EncoderFactory>();
	deps.video_decoder_factory = std::make_unique<KWebRtcH264DecoderFactory>();
	webrtc::EnableMedia(deps);
	m_spFactory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
	if (!m_spFactory)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Create WebRTC factory failed");
		return false;
	}

	return true;
}

bool KWebRtcPeer::createPeerConnection(QString *pErrorMessage)
{
	webrtc::PeerConnectionInterface::RTCConfiguration configuration;
	configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

	webrtc::PeerConnectionDependencies dependencies(this);
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>> result =
		m_spFactory->CreatePeerConnectionOrError(configuration, std::move(dependencies));
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = rtcErrorMessage(QStringLiteral("Create WebRTC peer failed"), result.error());
		return false;
	}

	m_spPeerConnection = result.value();
	return true;
}

bool KWebRtcPeer::createInputDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = true;
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>> result =
		m_spPeerConnection->CreateDataChannelOrError(kInputChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = rtcErrorMessage(QStringLiteral("Create input DataChannel failed"), result.error());
		return false;
	}

	m_pInputDataChannel->setChannel(result.value());
	return true;
}

bool KWebRtcPeer::createSessionDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = true;
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>> result =
		m_spPeerConnection->CreateDataChannelOrError(kSessionChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = rtcErrorMessage(QStringLiteral("Create session DataChannel failed"), result.error());
		return false;
	}

	m_pSessionDataChannel->setChannel(result.value());
	return true;
}

bool KWebRtcPeer::addLocalVideoTrack(QString *pErrorMessage)
{
	m_spVideoSource = webrtc::make_ref_counted<KWebRtcVideoSource>();
	webrtc::scoped_refptr<webrtc::VideoTrackInterface> spVideoTrack =
		m_spFactory->CreateVideoTrack(m_spVideoSource, kVideoLabel);

	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> result =
		m_spPeerConnection->AddTrack(spVideoTrack, { kStreamId });
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = rtcErrorMessage(QStringLiteral("Add WebRTC video track failed"), result.error());
		return false;
	}

	m_spVideoSender = result.value();
	return true;
}

bool KWebRtcPeer::addRemoteVideoReceiver(QString *pErrorMessage)
{
	webrtc::RtpTransceiverInit init;
	init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>> result =
		m_spPeerConnection->AddTransceiver(webrtc::MediaType::VIDEO, init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = rtcErrorMessage(QStringLiteral("Add WebRTC receive video transceiver failed"), result.error());
		return false;
	}

	return true;
}

void KWebRtcPeer::sendSessionDescription(const webrtc::SessionDescriptionInterface *pDescription)
{
	std::string strSdp;
	pDescription->ToString(&strSdp);
	KLatencyTraceLogger::write(roleToString(m_role),
		QStringLiteral("sdp_playout_delay"),
		QStringLiteral("direction=local type=%1 present=%2")
			.arg(stringViewToQString(webrtc::SdpTypeToString(pDescription->GetType())))
			.arg(strSdp.find(kPlayoutDelayUri) != std::string::npos ? 1 : 0));

	QJsonObject object;
	object.insert(QString::fromLatin1(kMessageType),
		stringViewToQString(webrtc::SdpTypeToString(pDescription->GetType())));
	object.insert(QString::fromLatin1(kSdpType),
		stringViewToQString(webrtc::SdpTypeToString(pDescription->GetType())));
	object.insert(QString::fromLatin1(kSdp), QString::fromStdString(strSdp));
	emit signalingMessageReady(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KWebRtcPeer::handleSessionDescription(const QString &strType, const QString &strSdp)
{
	KLatencyTraceLogger::write(roleToString(m_role),
		QStringLiteral("sdp_playout_delay"),
		QStringLiteral("direction=remote type=%1 present=%2")
			.arg(strType)
			.arg(strSdp.contains(QString::fromLatin1(kPlayoutDelayUri)) ? 1 : 0));

	std::optional<webrtc::SdpType> sdpType = webrtc::SdpTypeFromString(strType.toStdString());
	if (!sdpType.has_value())
	{
		emit transportError(QStringLiteral("Unknown SDP type"));
		return;
	}

	webrtc::SdpParseError parseError;
	std::unique_ptr<webrtc::SessionDescriptionInterface> spDescription =
		webrtc::CreateSessionDescription(sdpType.value(), strSdp.toStdString(), &parseError);
	if (!spDescription)
	{
		emit transportError(QString::fromStdString(parseError.description));
		return;
	}

	const webrtc::SdpType value = sdpType.value();
	m_spPeerConnection->SetRemoteDescription(KSetDescriptionObserver::create(
			[this, value]()
			{
				handleRemoteDescriptionSuccess(value);
			},
			[this](webrtc::RTCError error)
			{
				handleRemoteDescriptionFailure(error);
			})
			.get(),
		spDescription.release());
}

void KWebRtcPeer::handleIceCandidate(const QString &strSdpMid,
	int nSdpMLineIndex,
	const QString &strCandidate)
{
	webrtc::SdpParseError parseError;
	std::unique_ptr<webrtc::IceCandidate> spCandidate(webrtc::CreateIceCandidate(strSdpMid.toStdString(),
		nSdpMLineIndex,
		strCandidate.toStdString(),
		&parseError));
	if (!spCandidate)
	{
		emit transportError(QString::fromStdString(parseError.description));
		return;
	}

	if (!m_spPeerConnection->AddIceCandidate(spCandidate.get()))
		emit transportError(QStringLiteral("Add ICE candidate failed"));
}

QString KWebRtcPeer::rtcErrorMessage(const QString &strPrefix, const webrtc::RTCError &error)
{
	const absl::string_view strMessage = error.message();
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromUtf8(strMessage.data(), static_cast<int>(strMessage.size())));
}
