#include "transport/webrtc/webrtcpeer.h"

#include "common/framewatermark.h"
#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "transport/webrtc/webrtch264encoder.h"

#include <QtCore/QDateTime>
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
#include <api/video_codecs/video_decoder_factory_template.h>
#include <api/video_codecs/video_decoder_factory_template_open_h264_adapter.h>
#include <api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h>
#include <api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h>
#include <pc/video_track_source.h>
#include <rtc_base/logging.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <limits>
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
	constexpr int kExcellentRttMs = 20;
	constexpr int kGoodRttMs = 60;
	constexpr int kExcellentJitterMs = 10;
	constexpr int kGoodJitterMs = 30;
	constexpr double kExcellentPacketLossRate = 0.01;
	constexpr double kGoodPacketLossRate = 0.03;
	constexpr int kBitsPerByte = 8;
	constexpr int kMsPerSecond = 1000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr int kReceiverMaxFrameRateFps = 60;
	constexpr double kSecondsToMilliseconds = 1000.0;
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr quint64 kRemoteCallbackFrameCoalesceTraceInterval = 30;
	constexpr char kInputMouseMove[] = "mouseMove";
	constexpr char kInputMouseButton[] = "mouseButton";
	constexpr char kInputMouseWheel[] = "mouseWheel";
	constexpr char kInputType[] = "type";
	constexpr char kInputSeq[] = "seq";
	constexpr char kInputTrace[] = "trace";
	constexpr char kInputClientSendMs[] = "clientSendMs";
	constexpr char kLatencyPing[] = "latencyPing";
	constexpr char kLatencyPong[] = "latencyPong";
	constexpr char kLatencyId[] = "id";
	constexpr char kLatencySendMs[] = "sendMs";

	static int secondsToMs(double fSeconds)
	{
		return static_cast<int>(std::round(fSeconds * kSecondsToMilliseconds));
	}

	static QString evaluateNetworkQuality(const KWebRtcNetworkStats &stats)
	{
		if (stats.nRttMs < 0)
			return QStringLiteral("unknown");

		if (stats.nRttMs < kExcellentRttMs
			&& stats.fPacketLossRate < kExcellentPacketLossRate
			&& stats.nJitterMs < kExcellentJitterMs)
		{
			return QStringLiteral("excellent");
		}

		if (stats.nRttMs < kGoodRttMs
			&& stats.fPacketLossRate < kGoodPacketLossRate
			&& stats.nJitterMs < kGoodJitterMs)
		{
			return QStringLiteral("good");
		}

		return QStringLiteral("poor");
	}

	static QString stringViewToQString(absl::string_view value)
	{
		return QString::fromLatin1(value.data(), static_cast<int>(value.size()));
	}

	static QString roleToString(KWebRtcPeer::Role role)
	{
		return role == KWebRtcPeer::ControllerRole
			? QStringLiteral("controller")
			: QStringLiteral("controlled");
	}

	static QString sessionMessageType(const QString &strMessage)
	{
		const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
		if (!document.isObject())
			return QStringLiteral("invalid");

		const QString strType = document.object().value(QStringLiteral("type")).toString();
		return strType.isEmpty() ? QStringLiteral("unknown") : strType;
	}

	static QJsonObject jsonObjectFromMessage(const QString &strMessage)
	{
		const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
		return document.isObject() ? document.object() : QJsonObject();
	}

	static bool shouldTraceInputMessage(const QJsonObject &object)
	{
		return object.value(QString::fromLatin1(kInputTrace)).toBool(false);
	}

	static bool isLatencyMessageType(const QString &strType)
	{
		return strType == QString::fromLatin1(kLatencyPing)
			|| strType == QString::fromLatin1(kLatencyPong);
	}

	static QString inputTraceExtra(const QJsonObject &object)
	{
		return QStringLiteral("seq=%1 type=%2")
			.arg(object.value(QString::fromLatin1(kInputSeq)).toString())
			.arg(object.value(QString::fromLatin1(kInputType)).toString());
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

		void pushFrame(const KWebRtcVideoFrame &frame)
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

			for (int y = 0; y < nHeight; ++y)
			{
				std::memcpy(pDst + y * nDstStride,
					pSrc + y * nSrcStride,
					static_cast<size_t>(nWidth));
			}
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

	void pushFrame(const KWebRtcVideoFrame &frame)
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
	: QObject(pParent)
{
}

KWebRtcPeer::~KWebRtcPeer()
{
	shutdown();
}

bool KWebRtcPeer::initialize(Role role, QString *pErrorMessage)
{
	shutdown();
	m_role = role;
	if (!createFactory(pErrorMessage))
		return false;
	if (!createPeerConnection(pErrorMessage))
		return false;
	if (m_role == ControllerRole && !createInputDataChannel(pErrorMessage))
		return false;
	if (m_role == ControllerRole && !createSessionDataChannel(pErrorMessage))
		return false;
	if (m_role == ControlledRole && !addLocalVideoTrack(pErrorMessage))
		return false;
	if (m_role == ControllerRole && !addRemoteVideoReceiver(pErrorMessage))
		return false;

	emit stateChanged(QStringLiteral("PeerReady"));
	return true;
}

void KWebRtcPeer::shutdown()
{
	stopStatsPolling();
	if (m_spInputDataChannel)
	{
		m_spInputDataChannel->UnregisterObserver();
		m_spInputDataChannel = nullptr;
		emit inputChannelChanged(false);
	}
	if (m_spSessionDataChannel)
	{
		m_spSessionDataChannel->UnregisterObserver();
		m_spSessionDataChannel = nullptr;
		emit sessionChannelChanged(false);
	}

	if (m_spRemoteVideoTrack)
		m_spRemoteVideoTrack->RemoveSink(this);
	m_spRemoteVideoTrack = nullptr;
	clearPendingRemoteFrame();
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

void KWebRtcPeer::pushVideoFrame(const KWebRtcVideoFrame &frame)
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

void KWebRtcPeer::sendInputMessage(const QString &strMessage)
{
	if (!m_spInputDataChannel || m_spInputDataChannel->state() != webrtc::DataChannelInterface::kOpen)
		return;

	const QByteArray utf8Message = strMessage.toUtf8();
	const QJsonObject object = jsonObjectFromMessage(strMessage);
	if (shouldTraceInputMessage(object))
	{
		KLatencyTraceLogger::write(roleToString(m_role),
			QStringLiteral("input_send"),
			QStringLiteral("%1 size=%2 buffered=%3")
				.arg(inputTraceExtra(object))
				.arg(utf8Message.size())
				.arg(static_cast<qulonglong>(m_spInputDataChannel->buffered_amount())));
	}

	webrtc::DataBuffer buffer(std::string(utf8Message.constData(),
			static_cast<size_t>(utf8Message.size())));
	m_spInputDataChannel->Send(buffer);
}

void KWebRtcPeer::sendLatencyPing()
{
	if (m_role != ControllerRole
		|| !m_spInputDataChannel
		|| m_spInputDataChannel->state() != webrtc::DataChannelInterface::kOpen)
	{
		return;
	}

	QJsonObject object;
	object.insert(QString::fromLatin1(kInputType), QString::fromLatin1(kLatencyPing));
	object.insert(QString::fromLatin1(kLatencyId), QString::number(++m_nLatencyPingId));
	object.insert(QString::fromLatin1(kLatencySendMs), QDateTime::currentMSecsSinceEpoch());

	const QByteArray utf8Message = QJsonDocument(object).toJson(QJsonDocument::Compact);
	webrtc::DataBuffer buffer(std::string(utf8Message.constData(),
			static_cast<size_t>(utf8Message.size())));
	m_spInputDataChannel->Send(buffer);
}

void KWebRtcPeer::handleLatencyPing(const QJsonObject &object)
{
	if (m_role != ControlledRole
		|| !m_spInputDataChannel
		|| m_spInputDataChannel->state() != webrtc::DataChannelInterface::kOpen)
	{
		return;
	}

	QJsonObject response;
	response.insert(QString::fromLatin1(kInputType), QString::fromLatin1(kLatencyPong));
	response.insert(QString::fromLatin1(kLatencyId), object.value(QString::fromLatin1(kLatencyId)));
	response.insert(QString::fromLatin1(kLatencySendMs), object.value(QString::fromLatin1(kLatencySendMs)));

	const QByteArray utf8Message = QJsonDocument(response).toJson(QJsonDocument::Compact);
	webrtc::DataBuffer buffer(std::string(utf8Message.constData(),
			static_cast<size_t>(utf8Message.size())));
	m_spInputDataChannel->Send(buffer);
}

void KWebRtcPeer::handleLatencyPong(const QJsonObject &object)
{
	if (m_role != ControllerRole)
		return;

	const qint64 nSendMs =
		static_cast<qint64>(object.value(QString::fromLatin1(kLatencySendMs)).toDouble(-1));
	if (nSendMs < 0)
		return;

	m_nDataChannelRttMs = static_cast<int>(QDateTime::currentMSecsSinceEpoch() - nSendMs);
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("datachannel_rtt"),
		QStringLiteral("id=%1 rttMs=%2")
			.arg(object.value(QString::fromLatin1(kLatencyId)).toString())
			.arg(m_nDataChannelRttMs));
}

void KWebRtcPeer::sendSessionMessage(const QString &strMessage)
{
	if (!m_spSessionDataChannel || m_spSessionDataChannel->state() != webrtc::DataChannelInterface::kOpen)
	{
		KSessionTraceLogger::write(roleToString(m_role),
			QStringLiteral("send_drop"),
			sessionMessageType(strMessage),
			strMessage.toUtf8().size(),
			QStringLiteral("reason=session_channel_not_open"));
		return;
	}

	const QByteArray utf8Message = strMessage.toUtf8();
	KSessionTraceLogger::write(roleToString(m_role),
		QStringLiteral("send"),
		sessionMessageType(strMessage),
		utf8Message.size());
	webrtc::DataBuffer buffer(std::string(utf8Message.constData(),
			static_cast<size_t>(utf8Message.size())));
	m_spSessionDataChannel->Send(buffer);
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
		emit peerError(rtcErrorMessage(QStringLiteral("Set video stream parameters failed"), result));
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

	if (m_role != ControllerRole)
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
	KWebRtcNetworkStats stats;
	emit networkStatsReady(stats);
}

void KWebRtcPeer::requestStats()
{
	if (m_role != ControllerRole || !m_spPeerConnection || !m_spSignalingThread)
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
	m_bHasPreviousStats = false;
	m_nPreviousStatsMs = 0;
	m_nPreviousBytesReceived = 0;
	m_nPreviousPacketsReceived = 0;
	m_nPreviousPacketsLost = 0;
	m_fPreviousJitterBufferDelay = 0.0;
	m_fPreviousJitterBufferTargetDelay = 0.0;
	m_nPreviousJitterBufferEmittedCount = 0;
	m_fPreviousTotalDecodeTime = 0.0;
	m_nPreviousFramesDecoded = 0;
	m_nPreviousKeyFramesDecoded = 0;
	m_nPreviousFramesDropped = 0;
	m_nDataChannelRttMs = -1;
}

void KWebRtcPeer::handleStatsReport(webrtc::scoped_refptr<const webrtc::RTCStatsReport> spReport)
{
	if (!spReport || m_role != ControllerRole)
		return;

	KWebRtcNetworkStats stats;
	const qint64 nReportMs = spReport->timestamp().ms();

	quint64 nBytesReceived = 0;
	qint64 nPacketsReceived = 0;
	qint64 nPacketsLost = 0;
	double fJitterBufferDelay = 0.0;
	double fJitterBufferTargetDelay = 0.0;
	quint64 nJitterBufferEmittedCount = 0;
	double fTotalDecodeTime = 0.0;
	qint64 nFramesDecoded = 0;
	qint64 nKeyFramesDecoded = 0;
	qint64 nFramesDropped = 0;
	bool bHasInboundVideo = false;

	for (const webrtc::RTCInboundRtpStreamStats *pInbound :
		spReport->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>())
	{
		if (pInbound->kind && *pInbound->kind != "video")
			continue;

		bHasInboundVideo = true;
		if (pInbound->bytes_received)
			nBytesReceived = *pInbound->bytes_received;
		if (pInbound->packets_received)
			nPacketsReceived = *pInbound->packets_received;
		if (pInbound->packets_lost)
			nPacketsLost = *pInbound->packets_lost;
		if (pInbound->jitter)
			stats.nJitterMs = secondsToMs(*pInbound->jitter);
		if (pInbound->frames_per_second)
			stats.nFps = static_cast<int>(std::round(*pInbound->frames_per_second));
		if (pInbound->jitter_buffer_delay)
			fJitterBufferDelay = *pInbound->jitter_buffer_delay;
		if (pInbound->jitter_buffer_target_delay)
			fJitterBufferTargetDelay = *pInbound->jitter_buffer_target_delay;
		if (pInbound->jitter_buffer_emitted_count)
			nJitterBufferEmittedCount = *pInbound->jitter_buffer_emitted_count;
		if (pInbound->total_decode_time)
			fTotalDecodeTime = *pInbound->total_decode_time;
		if (pInbound->frames_decoded)
			nFramesDecoded = *pInbound->frames_decoded;
		if (pInbound->key_frames_decoded)
			nKeyFramesDecoded = *pInbound->key_frames_decoded;
		if (pInbound->frames_dropped)
			nFramesDropped = *pInbound->frames_dropped;
		break;
	}

	for (const webrtc::RTCIceCandidatePairStats *pCandidatePair :
		spReport->GetStatsOfType<webrtc::RTCIceCandidatePairStats>())
	{
		const bool bSucceeded = pCandidatePair->state && *pCandidatePair->state == "succeeded";
		const bool bNominated = pCandidatePair->nominated && *pCandidatePair->nominated;
		if (!bSucceeded || !bNominated || !pCandidatePair->current_round_trip_time)
			continue;

		stats.nRttMs = secondsToMs(*pCandidatePair->current_round_trip_time);
		break;
	}

	if (bHasInboundVideo && m_bHasPreviousStats && nReportMs > m_nPreviousStatsMs)
	{
		const qint64 nElapsedMs = nReportMs - m_nPreviousStatsMs;
		if (nBytesReceived >= m_nPreviousBytesReceived)
		{
			const quint64 nBytesDelta = nBytesReceived - m_nPreviousBytesReceived;
			stats.nBitrateKbps = static_cast<int>(
				(nBytesDelta * kBitsPerByte * kMsPerSecond)
				/ (static_cast<quint64>(nElapsedMs) * kBitsPerKilobit));
		}

		const qint64 nReceivedDelta = nPacketsReceived - m_nPreviousPacketsReceived;
		const qint64 nLostDelta = nPacketsLost - m_nPreviousPacketsLost;
		const qint64 nTotalPacketsDelta = std::max<qint64>(0, nReceivedDelta) + std::max<qint64>(0, nLostDelta);
		if (nTotalPacketsDelta > 0)
			stats.fPacketLossRate = static_cast<double>(std::max<qint64>(0, nLostDelta))
				/ static_cast<double>(nTotalPacketsDelta);

		const quint64 nJitterEmittedDelta =
			nJitterBufferEmittedCount >= m_nPreviousJitterBufferEmittedCount
			? nJitterBufferEmittedCount - m_nPreviousJitterBufferEmittedCount
			: 0;
		if (nJitterEmittedDelta > 0)
		{
			stats.nJitterBufferDelayMs = secondsToMs(
				std::max(0.0, fJitterBufferDelay - m_fPreviousJitterBufferDelay)
				/ static_cast<double>(nJitterEmittedDelta));
			stats.nJitterBufferTargetDelayMs = secondsToMs(
				std::max(0.0, fJitterBufferTargetDelay - m_fPreviousJitterBufferTargetDelay)
				/ static_cast<double>(nJitterEmittedDelta));
		}

		const qint64 nFramesDecodedDelta = nFramesDecoded - m_nPreviousFramesDecoded;
		if (nFramesDecodedDelta > 0)
		{
			stats.nDecodeTimeMs = secondsToMs(
				std::max(0.0, fTotalDecodeTime - m_fPreviousTotalDecodeTime)
				/ static_cast<double>(nFramesDecodedDelta));
		}
	}

	if (bHasInboundVideo)
	{
		m_bHasPreviousStats = true;
		m_nPreviousStatsMs = nReportMs;
		m_nPreviousBytesReceived = nBytesReceived;
		m_nPreviousPacketsReceived = nPacketsReceived;
		m_nPreviousPacketsLost = nPacketsLost;
		m_fPreviousJitterBufferDelay = fJitterBufferDelay;
		m_fPreviousJitterBufferTargetDelay = fJitterBufferTargetDelay;
		m_nPreviousJitterBufferEmittedCount = nJitterBufferEmittedCount;
		m_fPreviousTotalDecodeTime = fTotalDecodeTime;
		m_nPreviousFramesDecoded = nFramesDecoded;
		m_nPreviousKeyFramesDecoded = nKeyFramesDecoded;
		m_nPreviousFramesDropped = nFramesDropped;
	}

	if (stats.nJitterMs < 0)
		stats.nJitterMs = 0;
	stats.nDataChannelRttMs = m_nDataChannelRttMs;
	stats.nFramesDecoded = static_cast<int>(std::min<qint64>(nFramesDecoded, std::numeric_limits<int>::max()));
	stats.nKeyFramesDecoded = static_cast<int>(std::min<qint64>(nKeyFramesDecoded, std::numeric_limits<int>::max()));
	stats.nFramesDropped = static_cast<int>(std::min<qint64>(nFramesDropped, std::numeric_limits<int>::max()));
	stats.strQuality = evaluateNetworkQuality(stats);
	if (bHasInboundVideo)
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
	clearPendingRemoteFrame();
}

void KWebRtcPeer::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
{
	if (!channel)
		return;
	if (channel->label() == kInputChannelLabel)
		setInputDataChannel(channel);
	else if (channel->label() == kSessionChannelLabel)
		setSessionDataChannel(channel);
}

void KWebRtcPeer::OnRenegotiationNeeded()
{
}

void KWebRtcPeer::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state)
{
	emit stateChanged(stringViewToQString(webrtc::PeerConnectionInterface::AsString(new_state)));
	if (m_role != ControllerRole)
		return;

	if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted)
	{
		startStatsPolling(QStringLiteral("ice_connected"));
	}
	else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionClosed)
	{
		stopStatsPolling();
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

void KWebRtcPeer::OnStateChange()
{
	const bool bInputOpen = m_spInputDataChannel
		&& m_spInputDataChannel->state() == webrtc::DataChannelInterface::kOpen;
	const bool bSessionOpen = m_spSessionDataChannel
		&& m_spSessionDataChannel->state() == webrtc::DataChannelInterface::kOpen;
	emit inputChannelChanged(bInputOpen);
	emit sessionChannelChanged(bSessionOpen);
	emit stateChanged(bSessionOpen ? QStringLiteral("SessionChannelOpen") : QStringLiteral("SessionChannelClosed"));

	if (bInputOpen)
		startStatsPolling(QStringLiteral("input_channel_open"));
}

void KWebRtcPeer::OnMessage(const webrtc::DataBuffer &buffer)
{
	if (buffer.binary)
		return;

	const char *pData = reinterpret_cast<const char *>(buffer.data.data());
	const QString strMessage = QString::fromUtf8(pData, static_cast<int>(buffer.data.size()));
	const QJsonObject object = jsonObjectFromMessage(strMessage);
	const QString strMessageType = object.value(QString::fromLatin1(kInputType)).toString();
	if (isLatencyMessageType(strMessageType))
	{
		if (strMessageType == QString::fromLatin1(kLatencyPing))
			handleLatencyPing(object);
		else if (strMessageType == QString::fromLatin1(kLatencyPong))
			handleLatencyPong(object);
	}
	else if (isMouseInputMessage(strMessage))
	{
		if (shouldTraceInputMessage(object))
		{
			const qint64 nClientSendMs =
				static_cast<qint64>(object.value(QString::fromLatin1(kInputClientSendMs)).toDouble(-1));
			const qint64 nClientDelayMs = nClientSendMs >= 0
				? QDateTime::currentMSecsSinceEpoch() - nClientSendMs
				: -1;
			KLatencyTraceLogger::write(roleToString(m_role),
				QStringLiteral("input_recv"),
				QStringLiteral("%1 size=%2 clientDelayMs=%3")
					.arg(inputTraceExtra(object))
					.arg(static_cast<int>(buffer.data.size()))
					.arg(nClientDelayMs));
		}

		emit inputMessageReceived(strMessage);
	}
	else
	{
		KSessionTraceLogger::write(roleToString(m_role),
			QStringLiteral("recv"),
			sessionMessageType(strMessage),
			static_cast<int>(buffer.data.size()));
		emit sessionMessageReceived(strMessage);
	}
}

void KWebRtcPeer::OnBufferedAmountChange(uint64_t)
{
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
	emit peerError(rtcErrorMessage(QStringLiteral("Create SDP failed"), error));
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
	emit peerError(rtcErrorMessage(QStringLiteral("Set remote SDP failed"), error));
}

void KWebRtcPeer::OnFrame(const webrtc::VideoFrame &frame)
{
	bool bNeedQueue = false;
	quint64 nDroppedFrames = 0;
	{
		std::lock_guard<std::mutex> guard(m_remoteFrameMutex);
		if (m_bHasPendingRemoteFrame)
			++m_nDroppedRemoteCallbackFrames;

		m_pendingRemoteFrame = frame;
		m_bHasPendingRemoteFrame = true;
		nDroppedFrames = m_nDroppedRemoteCallbackFrames;
		if (!m_bRemoteFrameProcessQueued)
		{
			m_bRemoteFrameProcessQueued = true;
			bNeedQueue = true;
		}
	}

	if (nDroppedFrames > 0
		&& KLatencyTraceLogger::isEnabled()
		&& (nDroppedFrames == 1
			|| nDroppedFrames % kRemoteCallbackFrameCoalesceTraceInterval == 0))
	{
		KLatencyTraceLogger::write(roleToString(m_role),
			QStringLiteral("remote_callback_frame_coalesced"),
			QStringLiteral("dropped=%1 latestTimestampMs=%2")
				.arg(nDroppedFrames)
				.arg(frame.timestamp_us() / 1000));
	}

	if (bNeedQueue)
	{
		QMetaObject::invokeMethod(this,
			[this]()
			{
				processLatestRemoteFrame();
			},
			Qt::QueuedConnection);
	}
}

void KWebRtcPeer::processLatestRemoteFrame()
{
	std::optional<webrtc::VideoFrame> pendingFrame;
	{
		std::lock_guard<std::mutex> guard(m_remoteFrameMutex);
		if (!m_bHasPendingRemoteFrame)
		{
			m_bRemoteFrameProcessQueued = false;
			return;
		}

		pendingFrame = std::move(m_pendingRemoteFrame);
		m_pendingRemoteFrame.reset();
		m_bHasPendingRemoteFrame = false;
	}

	if (pendingFrame.has_value())
		decodeAndEmitRemoteFrame(*pendingFrame);

	bool bNeedQueue = false;
	{
		std::lock_guard<std::mutex> guard(m_remoteFrameMutex);
		if (m_bHasPendingRemoteFrame)
			bNeedQueue = true;
		else
			m_bRemoteFrameProcessQueued = false;
	}

	if (bNeedQueue)
	{
		QMetaObject::invokeMethod(this,
			[this]()
			{
				processLatestRemoteFrame();
			},
			Qt::QueuedConnection);
	}
}

void KWebRtcPeer::decodeAndEmitRemoteFrame(const webrtc::VideoFrame &frame)
{
	const quint64 nNextFrameIndex = m_nRemoteFrameIndex + 1;
	if (nNextFrameIndex % kVideoTraceFrameInterval == 0)
	{
		KLatencyTraceLogger::write(roleToString(m_role),
			QStringLiteral("remote_frame_recv"),
			QStringLiteral("frame=%1 timestampMs=%2")
				.arg(nNextFrameIndex)
				.arg(frame.timestamp_us() / 1000));
	}

	webrtc::scoped_refptr<webrtc::I420BufferInterface> spI420 = frame.video_frame_buffer()->ToI420();
	if (!spI420)
		return;

	const int nWidth = spI420->width();
	const int nHeight = spI420->height();
	KDecodedVideoFrame decodedFrame;
	decodedFrame.nWidth = nWidth;
	decodedFrame.nHeight = nHeight;
	decodedFrame.nFrameIndex = ++m_nRemoteFrameIndex;
	decodedFrame.nTimestampMs = frame.timestamp_us() / 1000;
	decodedFrame.vecBgraBuffer.resize(static_cast<size_t>(nWidth) * nHeight * 4);

	unsigned char *pDst = decodedFrame.vecBgraBuffer.data();
	for (int y = 0; y < nHeight; ++y)
	{
		const unsigned char *pY = spI420->DataY() + y * spI420->StrideY();
		const unsigned char *pU = spI420->DataU() + (y / 2) * spI420->StrideU();
		const unsigned char *pV = spI420->DataV() + (y / 2) * spI420->StrideV();
		unsigned char *pDstRow = pDst + static_cast<size_t>(y) * nWidth * 4;
		for (int x = 0; x < nWidth; ++x)
		{
			const int nY = std::max(0, static_cast<int>(pY[x]) - 16);
			const int nU = static_cast<int>(pU[x / 2]) - 128;
			const int nV = static_cast<int>(pV[x / 2]) - 128;
			const int nR = std::clamp((298 * nY + 409 * nV + 128) >> 8, 0, 255);
			const int nG = std::clamp((298 * nY - 100 * nU - 208 * nV + 128) >> 8, 0, 255);
			const int nB = std::clamp((298 * nY + 516 * nU + 128) >> 8, 0, 255);
			pDstRow[x * 4 + 0] = static_cast<unsigned char>(nB);
			pDstRow[x * 4 + 1] = static_cast<unsigned char>(nG);
			pDstRow[x * 4 + 2] = static_cast<unsigned char>(nR);
			pDstRow[x * 4 + 3] = 255;
		}
	}

	if (KLatencyTraceLogger::isEnabled())
	{
		KFrameWatermark watermark;
		if (KFrameWatermarkCodec::readBgra(decodedFrame.vecBgraBuffer,
				decodedFrame.nWidth,
				decodedFrame.nHeight,
				&watermark))
		{
			decodedFrame.nSourceFrameIndex = watermark.nSourceFrameIndex;
			decodedFrame.nLastInputSeq = watermark.nLastInputSeq;
			decodedFrame.nInputAgeMs = watermark.nInputAgeMs;
			if (decodedFrame.nFrameIndex > 0 && decodedFrame.nFrameIndex % kVideoTraceFrameInterval == 0)
			{
				KLatencyTraceLogger::write(QStringLiteral("controller"),
					QStringLiteral("remote_frame_trace"),
					QStringLiteral("frame=%1 sourceFrame=%2 lastInputSeq=%3 inputAgeMs=%4")
						.arg(decodedFrame.nFrameIndex)
						.arg(decodedFrame.nSourceFrameIndex)
						.arg(decodedFrame.nLastInputSeq)
						.arg(decodedFrame.nInputAgeMs));
			}
		}
	}

	emit remoteFrameReady(decodedFrame);
	emit remoteFrameStatsReady(decodedFrame.nWidth,
		decodedFrame.nHeight,
		decodedFrame.nFrameIndex,
		decodedFrame.nTimestampMs);
}

void KWebRtcPeer::clearPendingRemoteFrame()
{
	std::lock_guard<std::mutex> guard(m_remoteFrameMutex);
	m_pendingRemoteFrame.reset();
	m_bHasPendingRemoteFrame = false;
	m_bRemoteFrameProcessQueued = false;
	m_nDroppedRemoteCallbackFrames = 0;
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
	deps.video_decoder_factory = std::make_unique<webrtc::VideoDecoderFactoryTemplate<
		webrtc::OpenH264DecoderTemplateAdapter,
		webrtc::LibvpxVp8DecoderTemplateAdapter,
		webrtc::LibvpxVp9DecoderTemplateAdapter>>();
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

	setInputDataChannel(result.value());
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

	setSessionDataChannel(result.value());
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

void KWebRtcPeer::setInputDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel)
{
	if (m_spInputDataChannel)
		m_spInputDataChannel->UnregisterObserver();

	m_spInputDataChannel = spChannel;
	if (!m_spInputDataChannel)
	{
		emit inputChannelChanged(false);
		return;
	}

	m_spInputDataChannel->RegisterObserver(this);
	OnStateChange();
}

void KWebRtcPeer::setSessionDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel)
{
	if (m_spSessionDataChannel)
		m_spSessionDataChannel->UnregisterObserver();

	m_spSessionDataChannel = spChannel;
	if (!m_spSessionDataChannel)
	{
		emit sessionChannelChanged(false);
		return;
	}

	m_spSessionDataChannel->RegisterObserver(this);
	OnStateChange();
}

bool KWebRtcPeer::isMouseInputMessage(const QString &strMessage)
{
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return false;

	const QString strType = document.object().value(QStringLiteral("type")).toString();
	return strType == QStringLiteral("mouseMove")
		|| strType == QStringLiteral("mouseButton")
		|| strType == QStringLiteral("mouseWheel");
}

void KWebRtcPeer::sendSessionDescription(const webrtc::SessionDescriptionInterface *pDescription)
{
	std::string strSdp;
	pDescription->ToString(&strSdp);

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
	std::optional<webrtc::SdpType> sdpType = webrtc::SdpTypeFromString(strType.toStdString());
	if (!sdpType.has_value())
	{
		emit peerError(QStringLiteral("Unknown SDP type"));
		return;
	}

	webrtc::SdpParseError parseError;
	std::unique_ptr<webrtc::SessionDescriptionInterface> spDescription =
		webrtc::CreateSessionDescription(sdpType.value(), strSdp.toStdString(), &parseError);
	if (!spDescription)
	{
		emit peerError(QString::fromStdString(parseError.description));
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
		emit peerError(QString::fromStdString(parseError.description));
		return;
	}

	if (!m_spPeerConnection->AddIceCandidate(spCandidate.get()))
		emit peerError(QStringLiteral("Add ICE candidate failed"));
}

QString KWebRtcPeer::rtcErrorMessage(const QString &strPrefix, const webrtc::RTCError &error)
{
	const absl::string_view strMessage = error.message();
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromUtf8(strMessage.data(), static_cast<int>(strMessage.size())));
}
