#include "transport/webrtc/webrtcpeer.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "core/protocol/protocolconstraints.h"
#include "core/protocol/webrtcsignalingmessage.h"
#include "transport/webrtc/webrtcdatachannel.h"
#include "transport/webrtc/webrtccallbackgate.h"
#include "transport/webrtc/webrtch264decoder.h"
#include "transport/webrtc/webrtch264encoder.h"
#include "transport/webrtc/webrtclatencyprobe.h"
#include "transport/webrtc/webrtcremoteframeprocessor.h"

#include <QtCore/QElapsedTimer>
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
#include <common_video/include/video_frame_buffer.h>
#include <pc/video_track_source.h>
#include <rtc_base/logging.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

class KStatsCallback : public webrtc::RTCStatsCollectorCallback
{
public:
	KStatsCallback(std::shared_ptr<KWebRtcCallbackGate> spCallbackGate,
		quint64 nGeneration)
		: m_spCallbackGate(std::move(spCallbackGate))
		, m_nGeneration(nGeneration)
	{
	}

	void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &spReport) override
	{
		m_spCallbackGate->post(m_nGeneration,
			[spReport](QObject *pTarget)
			{
				static_cast<KWebRtcPeer *>(pTarget)->handleStatsReport(spReport);
			});
	}

private:
	std::shared_ptr<KWebRtcCallbackGate> m_spCallbackGate;
	quint64 m_nGeneration = 0;
};

namespace
{
	constexpr unsigned long kDestructorShutdownTimeoutMs = 2000;

	class KPeerInitializationGuard
	{
	public:
		explicit KPeerInitializationGuard(std::function<void()> rollback)
			: m_rollback(std::move(rollback))
		{
		}

		~KPeerInitializationGuard()
		{
			if (!m_bCommitted)
				m_rollback();
		}

		void commit()
		{
			m_bCommitted = true;
		}

	private:
		std::function<void()> m_rollback;
		bool m_bCommitted = false;
	};

	constexpr char kStreamId[] = "wrc-stream";
	constexpr char kVideoLabel[] = "wrc-screen";
	constexpr char kInputChannelLabel[] = "input";
	constexpr char kRealtimeInputChannelLabel[] = "input-realtime";
	constexpr char kSessionChannelLabel[] = "session";
	constexpr char kClipboardChannelLabel[] = "clipboard";
	constexpr char kTerminalChannelLabel[] = "terminal";
	constexpr char kFileTransferControlChannelLabel[] = "file-control";
	constexpr char kFileTransferDataChannelLabel[] = "file-data";
	constexpr int kStatsPollingIntervalMs = 1000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr double kSecondsToMilliseconds = 1000.0;
	constexpr quint64 kVideoTraceFrameInterval = 30;
	constexpr quint64 kInputLowWatermarkBytes = 1024;
	constexpr quint64 kInputHighWatermarkBytes = 4 * 1024;
	constexpr quint64 kRealtimeInputLowWatermarkBytes = 256;
	constexpr quint64 kRealtimeInputHighWatermarkBytes = 1024;
	constexpr quint64 kSessionLowWatermarkBytes = 64 * 1024;
	constexpr quint64 kSessionHighWatermarkBytes = 256 * 1024;
	constexpr quint64 kClipboardLowWatermarkBytes = 128 * 1024;
	constexpr quint64 kClipboardHighWatermarkBytes = 512 * 1024;
	constexpr quint64 kTerminalLowWatermarkBytes = 128 * 1024;
	constexpr quint64 kTerminalHighWatermarkBytes = 256 * 1024;
	constexpr quint64 kFileTransferLowWatermarkBytes = 256 * 1024;
	constexpr quint64 kFileTransferHighWatermarkBytes = 1024 * 1024;
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
			if (frame.nWidth <= 0 || frame.nHeight <= 0 || frame.spBuffer == nullptr)
				return;

			QElapsedTimer pushTimer;
			pushTimer.start();
			std::shared_ptr<KI420FrameBuffer> spFrameBuffer = frame.spBuffer;
			webrtc::scoped_refptr<webrtc::I420BufferInterface> spBuffer =
				webrtc::WrapI420Buffer(frame.nWidth,
					frame.nHeight,
					reinterpret_cast<const uint8_t *>(spFrameBuffer->yPlane.constData()),
					spFrameBuffer->nStrideY,
					reinterpret_cast<const uint8_t *>(spFrameBuffer->uPlane.constData()),
					spFrameBuffer->nStrideU,
					reinterpret_cast<const uint8_t *>(spFrameBuffer->vPlane.constData()),
					spFrameBuffer->nStrideV,
					[spFrameBuffer]() mutable { spFrameBuffer.reset(); });

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
			if (frame.nFrameIndex > 0
				&& frame.nFrameIndex % kVideoTraceFrameInterval == 0)
			{
				KLatencyTraceLogger::write(QStringLiteral("controlled"),
					QStringLiteral("webrtc_frame_delivered"),
					QStringLiteral("frame=%1 pushUs=%2 copyBytes=0")
						.arg(frame.nFrameIndex)
						.arg(pushTimer.nsecsElapsed() / 1000));
			}
		}

	private:
		struct SinkItem
		{
			webrtc::VideoSinkInterface<webrtc::VideoFrame> *pSink = nullptr;
			webrtc::VideoSinkWants wants;
		};

		std::mutex m_mutex;
		std::vector<SinkItem> m_vecSinks;
	};

	class KSetDescriptionObserver : public webrtc::SetSessionDescriptionObserver
	{
	public:
		using SuccessCallback = std::function<void(KWebRtcPeer *)>;
		using FailureCallback = std::function<void(KWebRtcPeer *, webrtc::RTCError)>;

		static webrtc::scoped_refptr<KSetDescriptionObserver> create(
			std::shared_ptr<KWebRtcCallbackGate> spCallbackGate,
			quint64 nGeneration,
			SuccessCallback successCallback = {},
			FailureCallback failureCallback = {})
		{
			return webrtc::make_ref_counted<KSetDescriptionObserver>(
				std::move(spCallbackGate), nGeneration, std::move(successCallback),
				std::move(failureCallback));
		}

		KSetDescriptionObserver(std::shared_ptr<KWebRtcCallbackGate> spCallbackGate,
			quint64 nGeneration,
			SuccessCallback successCallback,
			FailureCallback failureCallback)
			: m_spCallbackGate(std::move(spCallbackGate))
			, m_nGeneration(nGeneration)
			, m_successCallback(std::move(successCallback))
			, m_failureCallback(std::move(failureCallback))
		{
		}

		void OnSuccess() override
		{
			if (m_successCallback)
			{
				m_spCallbackGate->post(m_nGeneration,
					[callback = m_successCallback](QObject *pTarget)
					{
						callback(static_cast<KWebRtcPeer *>(pTarget));
					});
			}
		}

		void OnFailure(webrtc::RTCError error) override
		{
			if (m_failureCallback)
			{
				m_spCallbackGate->post(m_nGeneration,
					[callback = m_failureCallback, error](QObject *pTarget)
					{
						callback(static_cast<KWebRtcPeer *>(pTarget), error);
					});
			}
			RTC_LOG(LS_WARNING) << error.message();
		}

	private:
		std::shared_ptr<KWebRtcCallbackGate> m_spCallbackGate;
		quint64 m_nGeneration = 0;
		SuccessCallback m_successCallback;
		FailureCallback m_failureCallback;
	};
}

class KCreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver
{
public:
	static webrtc::scoped_refptr<KCreateSessionDescriptionObserver> create(
		std::shared_ptr<KWebRtcCallbackGate> spCallbackGate,
		quint64 nGeneration)
	{
		return webrtc::make_ref_counted<KCreateSessionDescriptionObserver>(
			std::move(spCallbackGate), nGeneration);
	}

	KCreateSessionDescriptionObserver(std::shared_ptr<KWebRtcCallbackGate> spCallbackGate,
		quint64 nGeneration)
		: m_spCallbackGate(std::move(spCallbackGate))
		, m_nGeneration(nGeneration)
	{
	}

	void OnSuccess(webrtc::SessionDescriptionInterface *pDescription) override
	{
		auto spDescription = std::make_shared<std::unique_ptr<webrtc::SessionDescriptionInterface>>(
			pDescription);
		m_spCallbackGate->post(m_nGeneration,
			[spDescription](QObject *pTarget)
			{
				std::unique_ptr<webrtc::SessionDescriptionInterface> upDescription =
					std::move(*spDescription);
				static_cast<KWebRtcPeer *>(pTarget)
					->handleLocalDescription(upDescription.release());
			});
	}

	void OnFailure(webrtc::RTCError error) override
	{
		m_spCallbackGate->post(m_nGeneration,
			[error](QObject *pTarget)
			{
				static_cast<KWebRtcPeer *>(pTarget)
					->handleLocalDescriptionFailure(error);
			});
	}

private:
	std::shared_ptr<KWebRtcCallbackGate> m_spCallbackGate;
	quint64 m_nGeneration = 0;
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
	: KWebRtcPeer(AutoVideoEncoderPreference, pParent)
{
}

KWebRtcPeer::KWebRtcPeer(KVideoEncoderPreference encoderPreference, QObject *pParent)
	: KRemotePeerTransport(pParent)
	, m_spCallbackGate(std::make_shared<KWebRtcCallbackGate>())
	, m_pInputDataChannel(new KWebRtcDataChannel(
		KProtocolConstraints::kMaximumInputMessageBytes, this))
	, m_pRealtimeInputDataChannel(new KWebRtcDataChannel(
		KProtocolConstraints::kMaximumInputMessageBytes, this))
	, m_pSessionDataChannel(new KWebRtcDataChannel(
		KProtocolConstraints::kMaximumSessionMessageBytes, this))
	, m_pClipboardDataChannel(new KWebRtcDataChannel(
		KProtocolConstraints::kMaximumClipboardMessageBytes, this))
	, m_pTerminalDataChannel(new KWebRtcDataChannel(64 * 1024, this))
	, m_pFileTransferControlDataChannel(new KWebRtcDataChannel(
		KProtocolConstraints::kMaximumFileControlMessageBytes, this))
	, m_pFileTransferDataDataChannel(new KWebRtcDataChannel(64 * 1024, this))
	, m_pRemoteFrameProcessor(new KWebRtcRemoteFrameProcessor(this))
	, m_spLatencyProbe(std::make_unique<KWebRtcLatencyProbe>())
	, m_encoderPreference(encoderPreference)
{
	const KProtocolRouter::Guard allowMessage =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &) { return true; };
	const KProtocolRouter::Guard allowControlledInput =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
		{
			return context.nRole == static_cast<int>(ControlledSessionRole);
		};
	const KProtocolRouter::Guard allowControllerMessage =
		[](const KProtocolEnvelope &, const KProtocolRouteContext &context)
		{
			return context.nRole == static_cast<int>(ControllerSessionRole);
		};
	for (KInputMessageType type : { MouseMoveInputMessageType, MouseButtonInputMessageType,
		MouseWheelInputMessageType, KeyInputMessageType, TextInputMessageType })
	{
		m_protocolRouter.registerHandler(InputProtocolChannel,
			KInputMessageCodec::typeName(type), allowControlledInput,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &context)
			{ return decodeInputMessage(envelope, context.bRealtimeInput); });
	}
	for (const QString &strType : { QStringLiteral("latencyPing"), QStringLiteral("latencyPong") })
	{
		m_protocolRouter.registerHandler(InputProtocolChannel, strType, allowMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return handleLatencyMessage(envelope); });
	}
	for (KSessionMessageType type : { DeviceInfoRequestSessionMessageType,
		StartStreamingSessionMessageType, StopStreamingSessionMessageType,
		StreamConfigSessionMessageType, SetPrivacyModeSessionMessageType,
		SetPostSessionActionSessionMessageType })
	{
		m_protocolRouter.registerHandler(SessionProtocolChannel,
			KSessionMessageCodec::typeName(type), allowControlledInput,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeSessionMessage(envelope); });
	}
	for (KSessionMessageType type : { DeviceInfoSessionMessageType,
		PrivacyModeStateSessionMessageType, PostSessionActionStateSessionMessageType })
	{
		m_protocolRouter.registerHandler(SessionProtocolChannel,
			KSessionMessageCodec::typeName(type), allowControllerMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeSessionMessage(envelope); });
	}
	m_protocolRouter.registerHandler(SessionProtocolChannel,
		KSessionMessageCodec::typeName(EndSessionMessageType), allowMessage,
		[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
		{ return decodeSessionMessage(envelope); });
	for (KSessionMessageType type : { CapabilitiesSessionMessageType,
		CapabilityRejectedSessionMessageType, CommandResultSessionMessageType })
	{
		m_protocolRouter.registerHandler(SessionProtocolChannel,
			KSessionMessageCodec::typeName(type), allowMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeSessionMessage(envelope); });
	}
	for (KClipboardMessageType type : { ReadyClipboardMessageType,
		TextClipboardMessageType, SyncStateClipboardMessageType })
	{
		m_protocolRouter.registerHandler(ClipboardProtocolChannel,
			KClipboardMessageCodec::typeName(type), allowMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeClipboardMessage(envelope); });
	}
	for (KTerminalMessageType type : { OpenRequestTerminalMessageType,
		ApprovalPendingTerminalMessageType, AcceptedTerminalMessageType,
		RejectedTerminalMessageType, ResizeTerminalMessageType,
		CloseTerminalMessageType, ExitedTerminalMessageType, ErrorTerminalMessageType,
		CommandResultTerminalMessageType })
	{
		m_protocolRouter.registerHandler(SessionProtocolChannel,
			KTerminalMessageCodec::typeName(type), allowMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeTerminalMessage(envelope); });
	}
	for (KFileTransferLifecycleMessageType type : {
		OpenRequestFileTransferLifecycleMessageType,
		OpenAcceptedFileTransferLifecycleMessageType,
		OpenRejectedFileTransferLifecycleMessageType,
		CloseFileTransferLifecycleMessageType,
		StoppedFileTransferLifecycleMessageType,
		ErrorFileTransferLifecycleMessageType })
	{
		m_protocolRouter.registerHandler(SessionProtocolChannel,
			KFileTransferLifecycleMessageCodec::typeName(type), allowMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeFileTransferLifecycleMessage(envelope); });
	}
	for (KFileTransferControlMessageType type : {
		ListRootsRequestFileTransferControlMessageType,
		ListRootsResponseFileTransferControlMessageType,
		ListDirectoryRequestFileTransferControlMessageType,
		ListDirectoryResponseFileTransferControlMessageType,
		CopyRequestFileTransferControlMessageType,
		CopyPlanBeginFileTransferControlMessageType,
		CopyPlanDirectoryFileTransferControlMessageType,
		CopyPlanEndFileTransferControlMessageType,
		FileBeginFileTransferControlMessageType,
		AckFileTransferControlMessageType,
		PauseFileTransferControlMessageType,
		ResumeFileTransferControlMessageType,
		CancelFileTransferControlMessageType,
		ConflictFileTransferControlMessageType,
		ConflictResolutionFileTransferControlMessageType,
		FileCompleteFileTransferControlMessageType,
		TaskCompleteFileTransferControlMessageType,
		ErrorFileTransferControlMessageType })
	{
		m_protocolRouter.registerHandler(FileControlProtocolChannel,
			KFileTransferControlMessageCodec::typeName(type), allowMessage,
			[this](const KProtocolEnvelope &envelope, const KProtocolRouteContext &)
			{ return decodeFileTransferControlMessage(envelope); });
	}

	m_pInputDataChannel->setBufferWatermarks(
		kInputLowWatermarkBytes, kInputHighWatermarkBytes);
	m_pRealtimeInputDataChannel->setBufferWatermarks(
		kRealtimeInputLowWatermarkBytes, kRealtimeInputHighWatermarkBytes);
	m_pSessionDataChannel->setBufferWatermarks(
		kSessionLowWatermarkBytes, kSessionHighWatermarkBytes);
	m_pClipboardDataChannel->setBufferWatermarks(
		kClipboardLowWatermarkBytes, kClipboardHighWatermarkBytes);
	m_pTerminalDataChannel->setBufferWatermarks(
		kTerminalLowWatermarkBytes, kTerminalHighWatermarkBytes);
	m_pFileTransferControlDataChannel->setBufferWatermarks(
		kFileTransferLowWatermarkBytes, kFileTransferHighWatermarkBytes);
	m_pFileTransferDataDataChannel->setBufferWatermarks(
		kFileTransferLowWatermarkBytes, kFileTransferHighWatermarkBytes);
	connect(m_pInputDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleInputChannelChanged);
	connect(m_pInputDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, &KWebRtcPeer::handleInputChannelMessage);
	connect(m_pInputDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("input"), nMessageBytes,
				strReason, &m_nInvalidInputMessages);
		});
	connect(m_pInputDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, &KWebRtcPeer::flushInputQueue);
	connect(m_pRealtimeInputDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleRealtimeInputChannelChanged);
	connect(m_pRealtimeInputDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, &KWebRtcPeer::handleRealtimeInputChannelMessage);
	connect(m_pRealtimeInputDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("input-realtime"), nMessageBytes,
				strReason, &m_nInvalidRealtimeInputMessages);
		});
	connect(m_pRealtimeInputDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, &KWebRtcPeer::flushRealtimeInputQueue);
	connect(m_pSessionDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleSessionChannelChanged);
	connect(m_pSessionDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, &KWebRtcPeer::handleSessionChannelMessage);
	connect(m_pSessionDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("session"), nMessageBytes,
				strReason, &m_nInvalidSessionMessages);
		});
	connect(m_pSessionDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, &KWebRtcPeer::flushSessionQueue);
	connect(m_pClipboardDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleClipboardChannelChanged);
	connect(m_pClipboardDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, &KWebRtcPeer::handleClipboardChannelMessage);
	connect(m_pClipboardDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("clipboard"), nMessageBytes,
				strReason, &m_nInvalidClipboardMessages);
		});
	connect(m_pClipboardDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, &KWebRtcPeer::flushClipboardQueue);
	connect(m_pTerminalDataChannel, &KWebRtcDataChannel::openChanged,
		this, &KWebRtcPeer::handleTerminalChannelChanged);
	connect(m_pTerminalDataChannel, &KWebRtcDataChannel::binaryMessageReceived,
		this, [this](const QByteArray &data)
		{ emit terminalDataReceived(m_nGeneration.load(), data); });
	connect(m_pTerminalDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("terminal"), nMessageBytes,
				strReason, &m_nInvalidTerminalMessages);
		});
	connect(m_pTerminalDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, [this]() { emit terminalLowWatermarkReached(m_nGeneration.load()); });
	connect(m_pFileTransferControlDataChannel, &KWebRtcDataChannel::openChanged,
		this, [this](bool) { handleFileTransferChannelChanged(); });
	connect(m_pFileTransferControlDataChannel, &KWebRtcDataChannel::textMessageReceived,
		this, [this](const QString &strMessage)
		{ routeDataMessage(FileControlProtocolChannel, strMessage,
			&m_nInvalidFileTransferControlMessages); });
	connect(m_pFileTransferControlDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("file-control"), nMessageBytes,
				strReason, &m_nInvalidFileTransferControlMessages);
		});
	connect(m_pFileTransferControlDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, [this]() { emit fileTransferLowWatermarkReached(m_nGeneration.load()); });
	connect(m_pFileTransferDataDataChannel, &KWebRtcDataChannel::openChanged,
		this, [this](bool) { handleFileTransferChannelChanged(); });
	connect(m_pFileTransferDataDataChannel, &KWebRtcDataChannel::binaryMessageReceived,
		this, [this](const QByteArray &data)
		{ emit fileTransferDataReceived(m_nGeneration.load(), data); });
	connect(m_pFileTransferDataDataChannel, &KWebRtcDataChannel::messageRejected,
		this, [this](int nMessageBytes, const QString &strReason)
		{
			handleProtocolReject(QStringLiteral("file-data"), nMessageBytes,
				strReason, &m_nInvalidFileTransferDataMessages);
		});
	connect(m_pFileTransferDataDataChannel, &KWebRtcDataChannel::lowWatermarkReached,
		this, [this]() { emit fileTransferLowWatermarkReached(m_nGeneration.load()); });
	connect(m_pRemoteFrameProcessor, &KWebRtcRemoteFrameProcessor::frameReady,
		this, [this](const KDecodedVideoFrame &frame)
		{ emit remoteFrameReady(m_nGeneration.load(), frame); });
	connect(m_pRemoteFrameProcessor, &KWebRtcRemoteFrameProcessor::frameStatsReady,
		this, [this](int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs)
		{ emit remoteFrameStatsReady(m_nGeneration.load(), nWidth, nHeight, nFrameIndex, nTimestampMs); });
}

KWebRtcPeer::~KWebRtcPeer()
{
	requestShutdown(m_nGeneration.load());
	QThread *pTeardownThread = m_pTeardownThread;
	if (pTeardownThread == nullptr)
		return;

	if (QThread::currentThread() != pTeardownThread
		&& pTeardownThread->wait(kDestructorShutdownTimeoutMs))
	{
		QObject::disconnect(pTeardownThread, nullptr, this, nullptr);
		m_pTeardownThread = nullptr;
		delete pTeardownThread;
		return;
	}

	m_spCallbackGate->close();
	qCritical() << "WebRTC teardown did not stop before destruction; isolating it";
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("webrtc_lifecycle"),
		QStringLiteral("destructor_timeout"), -1,
		QStringLiteral("timeoutMs=%1 generation=%2")
			.arg(kDestructorShutdownTimeoutMs)
			.arg(m_nGeneration.load()));
	QObject::disconnect(pTeardownThread, nullptr, this, nullptr);
	connect(pTeardownThread, &QThread::finished,
		pTeardownThread, &QObject::deleteLater);
	m_pTeardownThread = nullptr;
}

KPeerInitializationResult KWebRtcPeer::initialize(KSessionRole role,
	quint64 nGeneration)
{
	if (m_lifecycleState != IdleWebRtcPeerLifecycleState)
		return KPeerInitializationResult::rejected(
			QStringLiteral("WebRTC peer is not idle"));

	m_lifecycleState = InitializingWebRtcPeerLifecycleState;
	m_nGeneration = nGeneration;
	m_spCallbackGate->open(this, nGeneration);
	m_bProtocolTerminationPending = false;
	m_role = role;
	KPeerInitializationGuard guard(
		[this, nGeneration]() { requestShutdown(nGeneration); });
	QString strError;
	if (shouldFailInitializationAt(ThreadsPeerInitializationStage)
		|| !createThreads(&strError))
	{
		return failInitialization(ThreadsPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected WebRTC thread failure") : strError);
	}
	if (shouldFailInitializationAt(FactoryPeerInitializationStage)
		|| !createFactory(&strError))
	{
		return failInitialization(FactoryPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected WebRTC factory failure") : strError);
	}
	if (shouldFailInitializationAt(PeerConnectionPeerInitializationStage)
		|| !createPeerConnection(&strError))
	{
		return failInitialization(PeerConnectionPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected PeerConnection failure") : strError);
	}
	if (m_role == ControllerSessionRole
		&& (shouldFailInitializationAt(InputChannelPeerInitializationStage)
			|| !createInputDataChannel(&strError)))
	{
		return failInitialization(InputChannelPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected input channel failure") : strError);
	}
	if (m_role == ControllerSessionRole
		&& (shouldFailInitializationAt(RealtimeInputChannelPeerInitializationStage)
			|| !createRealtimeInputDataChannel(&strError)))
	{
		return failInitialization(RealtimeInputChannelPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected realtime input channel failure") : strError);
	}
	if (m_role == ControllerSessionRole
		&& (shouldFailInitializationAt(SessionChannelPeerInitializationStage)
			|| !createSessionDataChannel(&strError)))
	{
		return failInitialization(SessionChannelPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected session channel failure") : strError);
	}
	if (m_role == ControllerSessionRole
		&& (shouldFailInitializationAt(ClipboardChannelPeerInitializationStage)
			|| !createClipboardDataChannel(&strError)))
	{
		return failInitialization(ClipboardChannelPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected clipboard channel failure") : strError);
	}
	if (m_role == ControllerSessionRole
		&& (shouldFailInitializationAt(TerminalChannelPeerInitializationStage)
			|| !createTerminalDataChannel(&strError)))
	{
		return failInitialization(TerminalChannelPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected terminal channel failure") : strError);
	}
	if (m_role == ControlledSessionRole
		&& (shouldFailInitializationAt(LocalVideoTrackPeerInitializationStage)
			|| !addLocalVideoTrack(&strError)))
	{
		return failInitialization(LocalVideoTrackPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected local video track failure") : strError);
	}
	if (m_role == ControllerSessionRole
		&& (shouldFailInitializationAt(RemoteVideoReceiverPeerInitializationStage)
			|| !addRemoteVideoReceiver(&strError)))
	{
		return failInitialization(RemoteVideoReceiverPeerInitializationStage,
			strError.isEmpty() ? QStringLiteral("Injected remote video receiver failure") : strError);
	}

	m_lifecycleState = ReadyWebRtcPeerLifecycleState;
	guard.commit();
	emit stateChanged(m_nGeneration.load(), QStringLiteral("PeerReady"));
	return KPeerInitializationResult::success();
}

bool KWebRtcPeer::shouldFailInitializationAt(KPeerInitializationStage) const
{
	return false;
}

KPeerInitializationResult KWebRtcPeer::failInitialization(
	KPeerInitializationStage stage, const QString &strTechnicalMessage)
{
	m_lifecycleState = FailedWebRtcPeerLifecycleState;
	KSessionTraceLogger::write(KSessionStateMachine::roleName(m_role),
		QStringLiteral("peer_initialization"), QStringLiteral("failed"), -1,
		QStringLiteral("generation=%1 stage=%2 technical=%3")
			.arg(m_nGeneration.load())
			.arg(KPeerInitializationResult::stageName(stage), strTechnicalMessage));
	return KPeerInitializationResult::rollbackPending(stage, strTechnicalMessage);
}

quint64 KWebRtcPeer::generation() const
{
	return m_nGeneration.load();
}

bool KWebRtcPeer::postCallback(quint64 nGeneration,
	std::function<void(KWebRtcPeer *)> callback)
{
	return m_spCallbackGate->post(nGeneration,
		[callback = std::move(callback)](QObject *pTarget)
		{
			callback(static_cast<KWebRtcPeer *>(pTarget));
		});
}

void KWebRtcPeer::requestShutdown(quint64 nGeneration)
{
	if (m_lifecycleState == ShuttingDownWebRtcPeerLifecycleState)
		return;
	m_lifecycleState = ShuttingDownWebRtcPeerLifecycleState;

	m_spCallbackGate->close();
	stopStatsPolling();
	m_inputSendQueue.clear();
	m_realtimeInputSendQueue.clear();
	m_sessionSendQueue.clear();
	m_clipboardSendQueue.clear();
	m_nInvalidSignalingMessages = 0;
	m_nInvalidInputMessages = 0;
	m_nInvalidRealtimeInputMessages = 0;
	m_nInvalidSessionMessages = 0;
	m_nInvalidClipboardMessages = 0;
	m_nInvalidTerminalMessages = 0;
	m_nInvalidFileTransferControlMessages = 0;
	m_nInvalidFileTransferDataMessages = 0;
	m_bProtocolTerminationPending = false;
	m_bInputRealtimeEnabled = false;
	m_pInputDataChannel->clear();
	m_pRealtimeInputDataChannel->clear();
	m_pSessionDataChannel->clear();
	m_pClipboardDataChannel->clear();
	m_pTerminalDataChannel->clear();
	m_pFileTransferControlDataChannel->clear();
	m_pFileTransferDataDataChannel->clear();

	if (m_spPeerConnection)
		m_spPeerConnection->Close();
	if (m_spRemoteVideoTrack)
		m_spRemoteVideoTrack->RemoveSink(this);
	webrtc::scoped_refptr<webrtc::VideoTrackInterface> spRemoteVideoTrack = std::move(m_spRemoteVideoTrack);
	m_pRemoteFrameProcessor->clear();
	auto spVideoSender = std::move(m_spVideoSender);
	auto spPeerConnection = std::move(m_spPeerConnection);
	auto spVideoSource = std::move(m_spVideoSource);
	auto spFactory = std::move(m_spFactory);
	auto upSignalingThread = std::move(m_spSignalingThread);
	auto upWorkerThread = std::move(m_spWorkerThread);
	auto upNetworkThread = std::move(m_spNetworkThread);
	resetStatsHistory();

	if (!spRemoteVideoTrack && !spVideoSender && !spPeerConnection && !spVideoSource
		&& !spFactory && !upSignalingThread && !upWorkerThread && !upNetworkThread)
	{
		QMetaObject::invokeMethod(this,
			[this, nGeneration]()
			{
				if (m_lifecycleState != ShuttingDownWebRtcPeerLifecycleState)
					return;
				m_lifecycleState = IdleWebRtcPeerLifecycleState;
				emit shutdownFinished(nGeneration);
			}, Qt::QueuedConnection);
		return;
	}

	m_pTeardownThread = QThread::create(
		[spRemoteVideoTrack = std::move(spRemoteVideoTrack),
			spVideoSender = std::move(spVideoSender),
			spPeerConnection = std::move(spPeerConnection),
			spVideoSource = std::move(spVideoSource),
			spFactory = std::move(spFactory),
			upSignalingThread = std::move(upSignalingThread),
			upWorkerThread = std::move(upWorkerThread),
			upNetworkThread = std::move(upNetworkThread)]() mutable
		{
			spRemoteVideoTrack = nullptr;
			spVideoSender = nullptr;
			spPeerConnection = nullptr;
			spVideoSource = nullptr;
			spFactory = nullptr;
			if (upNetworkThread)
				upNetworkThread->Stop();
			if (upWorkerThread)
				upWorkerThread->Stop();
			if (upSignalingThread)
				upSignalingThread->Stop();
		});
	connect(m_pTeardownThread, &QThread::finished, this,
		[this, nGeneration]()
		{
			QThread *pFinishedThread = m_pTeardownThread;
			m_pTeardownThread = nullptr;
			m_lifecycleState = IdleWebRtcPeerLifecycleState;
			if (pFinishedThread != nullptr)
				pFinishedThread->deleteLater();
			emit shutdownFinished(nGeneration);
		});
	m_pTeardownThread->start();
}

void KWebRtcPeer::createOffer()
{
	if (!m_spPeerConnection)
		return;
	m_spPeerConnection->CreateOffer(KCreateSessionDescriptionObserver::create(
		m_spCallbackGate, m_nGeneration.load()).get(),
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void KWebRtcPeer::restartIce()
{
	if (!m_spPeerConnection)
		return;

	m_spPeerConnection->RestartIce();
	m_spPeerConnection->CreateOffer(KCreateSessionDescriptionObserver::create(
		m_spCallbackGate, m_nGeneration.load()).get(),
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void KWebRtcPeer::handleSignalingMessage(const KWebRtcSignalingMessage &message)
{
	if (message.type == OfferWebRtcSignalingMessageType
		|| message.type == AnswerWebRtcSignalingMessageType)
	{
		handleSessionDescription(KWebRtcSignalingMessageCodec::typeName(message.type), message.strSdp);
	}
	else
	{
		handleIceCandidate(message.strSdpMid, message.nSdpMLineIndex, message.strCandidate);
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
	const QString strPayload = KInputMessageCodec::encode(message);
	const bool bMouseMove = message.type == MouseMoveInputMessageType;
	if (bMouseMove
		&& m_bInputRealtimeEnabled
		&& m_pRealtimeInputDataChannel->isOpen())
	{
		KOutboundMessage queuedMessage {
			strPayload, QStringLiteral("mouseMove"), false
		};
		if (!m_realtimeInputSendQueue.isEmpty()
			|| m_pRealtimeInputDataChannel->isBackpressured())
		{
			m_realtimeInputSendQueue.enqueue(queuedMessage);
			return;
		}
		if (!m_pRealtimeInputDataChannel->sendText(strPayload))
			m_realtimeInputSendQueue.enqueue(queuedMessage);
		return;
	}
	if (!m_inputSendQueue.isEmpty() || m_pInputDataChannel->isBackpressured())
	{
		enqueueInputMessage(strPayload, bMouseMove);
		return;
	}
	if (!m_pInputDataChannel->sendText(strPayload))
		enqueueInputMessage(strPayload, bMouseMove);
}

void KWebRtcPeer::setInputRealtimeEnabled(bool bEnabled)
{
	m_bInputRealtimeEnabled = bEnabled;
	if (!bEnabled)
	{
		m_realtimeInputSendQueue.clear();
		return;
	}
	flushRealtimeInputQueue();
}

void KWebRtcPeer::sendLatencyPing()
{
	if (m_role != ControllerSessionRole
		|| !m_pInputDataChannel->isOpen())
	{
		return;
	}
	if (!m_pInputDataChannel->isBackpressured() && m_inputSendQueue.isEmpty())
		m_pInputDataChannel->sendText(m_spLatencyProbe->createPing());
}

void KWebRtcPeer::sendClipboardMessage(const KClipboardMessage &message)
{
	KOutboundMessage queuedMessage;
	queuedMessage.strPayload = KClipboardMessageCodec::encode(message);
	queuedMessage.strCoalescingKey = message.type == TextClipboardMessageType
		? QStringLiteral("clipboardText") : QString();
	queuedMessage.bReliable = message.type != TextClipboardMessageType;
	if (!m_clipboardSendQueue.isEmpty() || m_pClipboardDataChannel->isBackpressured())
	{
		m_clipboardSendQueue.enqueue(queuedMessage);
		return;
	}
	if (!m_pClipboardDataChannel->sendText(queuedMessage.strPayload))
		m_clipboardSendQueue.enqueue(queuedMessage);
}

KRemotePeerTransport::KSessionMessageSendStatus
KWebRtcPeer::sendTerminalControlMessage(const KTerminalMessage &message)
{
	KOutboundMessage queuedMessage;
	queuedMessage.strPayload = KTerminalMessageCodec::encode(message);
	queuedMessage.bReliable = true;
	if (!m_pSessionDataChannel->isOpen())
		return SessionMessageChannelUnavailable;
	if (!m_sessionSendQueue.isEmpty() || m_pSessionDataChannel->isBackpressured())
	{
		return m_sessionSendQueue.enqueue(queuedMessage) == OverflowOutboundMessage
			? SessionMessageQueueOverflow : SessionMessageAccepted;
	}
	if (m_pSessionDataChannel->sendText(queuedMessage.strPayload))
		return SessionMessageAccepted;
	return SessionMessageTransportFailed;
}

bool KWebRtcPeer::sendTerminalData(const QByteArray &data)
{
	return m_pTerminalDataChannel->sendBinary(data);
}

bool KWebRtcPeer::terminalBackpressured() const
{
	return m_pTerminalDataChannel->isBackpressured();
}

bool KWebRtcPeer::ensureFileTransferChannels()
{
	if (m_role != ControllerSessionRole
		|| m_lifecycleState != ReadyWebRtcPeerLifecycleState
		|| !m_spPeerConnection)
	{
		return false;
	}

	QString strError;
	if (!m_pFileTransferControlDataChannel->hasChannel()
		&& !createFileTransferControlDataChannel(&strError))
	{
		KSessionTraceLogger::write(roleToString(m_role),
			QStringLiteral("file_transfer_channel"), QStringLiteral("create_failed"),
			-1, QStringLiteral("channel=control error=%1").arg(strError));
		return false;
	}
	if (!m_pFileTransferDataDataChannel->hasChannel()
		&& !createFileTransferDataDataChannel(&strError))
	{
		m_pFileTransferControlDataChannel->clear();
		KSessionTraceLogger::write(roleToString(m_role),
			QStringLiteral("file_transfer_channel"), QStringLiteral("create_failed"),
			-1, QStringLiteral("channel=data error=%1").arg(strError));
		return false;
	}
	return true;
}

KRemotePeerTransport::KSessionMessageSendStatus
KWebRtcPeer::sendFileTransferLifecycleMessage(
	const KFileTransferLifecycleMessage &message)
{
	KOutboundMessage queuedMessage;
	queuedMessage.strPayload = KFileTransferLifecycleMessageCodec::encode(message);
	queuedMessage.bReliable = true;
	if (!m_pSessionDataChannel->isOpen())
		return SessionMessageChannelUnavailable;
	if (!m_sessionSendQueue.isEmpty() || m_pSessionDataChannel->isBackpressured())
	{
		return m_sessionSendQueue.enqueue(queuedMessage) == OverflowOutboundMessage
			? SessionMessageQueueOverflow : SessionMessageAccepted;
	}
	return m_pSessionDataChannel->sendText(queuedMessage.strPayload)
		? SessionMessageAccepted : SessionMessageTransportFailed;
}

bool KWebRtcPeer::sendFileTransferControlMessage(
	const KFileTransferControlMessage &message)
{
	if (m_pFileTransferControlDataChannel->isBackpressured())
		return false;
	return m_pFileTransferControlDataChannel->sendText(
		KFileTransferControlMessageCodec::encode(message));
}

bool KWebRtcPeer::sendFileTransferData(const QByteArray &data)
{
	if (m_pFileTransferDataDataChannel->isBackpressured())
		return false;
	return m_pFileTransferDataDataChannel->sendBinary(data);
}

bool KWebRtcPeer::fileTransferBackpressured() const
{
	return m_pFileTransferControlDataChannel->isBackpressured()
		|| m_pFileTransferDataDataChannel->isBackpressured();
}

bool KWebRtcPeer::enqueueInputMessage(const QString &strPayload, bool bMouseMove)
{
	KOutboundMessage queuedMessage;
	queuedMessage.strPayload = strPayload;
	queuedMessage.strCoalescingKey = bMouseMove ? QStringLiteral("mouseMove") : QString();
	queuedMessage.bReliable = !bMouseMove;
	const KOutboundEnqueueResult result = m_inputSendQueue.enqueue(queuedMessage);
	if (result != OverflowOutboundMessage)
		return true;
	emit inputBackpressureOverflow(m_nGeneration.load());
	return false;
}

void KWebRtcPeer::flushInputQueue()
{
	while (m_pInputDataChannel->isOpen()
		&& !m_pInputDataChannel->isBackpressured()
		&& !m_inputSendQueue.isEmpty())
	{
		KOutboundMessage message;
		if (!m_inputSendQueue.takeFirst(&message))
			return;
		if (!m_pInputDataChannel->sendText(message.strPayload))
		{
			m_inputSendQueue.prepend(message);
			return;
		}
	}
}

void KWebRtcPeer::flushRealtimeInputQueue()
{
	while (m_bInputRealtimeEnabled
		&& m_pRealtimeInputDataChannel->isOpen()
		&& !m_pRealtimeInputDataChannel->isBackpressured()
		&& !m_realtimeInputSendQueue.isEmpty())
	{
		KOutboundMessage message;
		if (!m_realtimeInputSendQueue.takeFirst(&message))
			return;
		if (!m_pRealtimeInputDataChannel->sendText(message.strPayload))
		{
			m_realtimeInputSendQueue.prepend(message);
			return;
		}
	}
}

void KWebRtcPeer::flushClipboardQueue()
{
	while (m_pClipboardDataChannel->isOpen()
		&& !m_pClipboardDataChannel->isBackpressured()
		&& !m_clipboardSendQueue.isEmpty())
	{
		KOutboundMessage message;
		if (!m_clipboardSendQueue.takeFirst(&message))
			return;
		if (!m_pClipboardDataChannel->sendText(message.strPayload))
		{
			m_clipboardSendQueue.prepend(message);
			return;
		}
	}
}

void KWebRtcPeer::flushSessionQueue()
{
	while (m_pSessionDataChannel->isOpen()
		&& !m_pSessionDataChannel->isBackpressured()
		&& !m_sessionSendQueue.isEmpty())
	{
		KOutboundMessage message;
		if (!m_sessionSendQueue.takeFirst(&message))
			return;
		if (!m_pSessionDataChannel->sendText(message.strPayload))
		{
			m_sessionSendQueue.prepend(message);
			return;
		}
	}
}

KRemotePeerTransport::KSessionMessageSendStatus KWebRtcPeer::sendSessionMessage(
	const KSessionMessage &message)
{
	const QString strPayload = KSessionMessageCodec::encode(message);
	if (!m_pSessionDataChannel->isOpen())
		return SessionMessageChannelUnavailable;
	if (!m_pSessionDataChannel->isBackpressured()
		&& m_sessionSendQueue.isEmpty())
	{
		return m_pSessionDataChannel->sendText(strPayload)
			? SessionMessageAccepted : SessionMessageTransportFailed;
	}

	const KOutboundEnqueueResult result = m_sessionSendQueue.enqueue({
		strPayload, KSessionMessageCodec::typeName(message.type), true });
	if (result == OverflowOutboundMessage)
	{
		return SessionMessageQueueOverflow;
	}
	return SessionMessageAccepted;
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
		emit transportError(m_nGeneration.load(), rtcErrorMessage(QStringLiteral("Set video stream parameters failed"), result));
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
	emit networkStatsReady(m_nGeneration.load(), stats);
}

void KWebRtcPeer::requestStats()
{
	if (m_role != ControllerSessionRole || !m_spPeerConnection || !m_spSignalingThread)
		return;

	sendLatencyPing();

	webrtc::scoped_refptr<KStatsCallback> spCallback =
		webrtc::make_ref_counted<KStatsCallback>(m_spCallbackGate,
			m_nGeneration.load());
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
			QStringLiteral("jitterBufferMs=%1 jitterTargetEstimateMs=%2 decodeMs=%3 framesDecoded=%4 keyFramesDecoded=%5 dropped=%6")
				.arg(stats.nJitterBufferDelayMs)
				.arg(stats.nJitterBufferTargetDelayMs)
				.arg(stats.nDecodeTimeMs)
				.arg(stats.nFramesDecoded)
				.arg(stats.nKeyFramesDecoded)
				.arg(stats.nFramesDropped));
	}
	emit networkStatsReady(m_nGeneration.load(), stats);
}

void KWebRtcPeer::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState)
{
}

void KWebRtcPeer::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
	const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>> &streams)
{
	if (QThread::currentThread() != thread())
	{
		postCallback(m_nGeneration.load(),
			[receiver, streams](KWebRtcPeer *pPeer)
			{
				pPeer->OnAddTrack(receiver, streams);
			});
		return;
	}
	if (!receiver || !receiver->track()
		|| receiver->track()->kind() != webrtc::MediaStreamTrackInterface::kVideoKind)
	{
		return;
	}

	m_spRemoteVideoTrack = static_cast<webrtc::VideoTrackInterface *>(receiver->track().get());
	webrtc::VideoSinkWants wants;
	wants.black_frames = false;
	wants.max_framerate_fps = KProtocolConstraints::kMaximumStreamFps;
	m_spRemoteVideoTrack->AddOrUpdateSink(this, wants);
	emit stateChanged(m_nGeneration.load(), QStringLiteral("RemoteVideoTrack"));
	startStatsPolling(QStringLiteral("remote_video_track"));
}

void KWebRtcPeer::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver)
{
	if (QThread::currentThread() != thread())
	{
		postCallback(m_nGeneration.load(),
			[receiver](KWebRtcPeer *pPeer)
			{
				pPeer->OnRemoveTrack(receiver);
			});
		return;
	}
	if (m_spRemoteVideoTrack)
		m_spRemoteVideoTrack->RemoveSink(this);
	m_spRemoteVideoTrack = nullptr;
	m_pRemoteFrameProcessor->clear();
}

void KWebRtcPeer::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel)
{
	if (QThread::currentThread() != thread())
	{
		postCallback(m_nGeneration.load(),
			[channel](KWebRtcPeer *pPeer)
			{
				pPeer->OnDataChannel(channel);
			});
		return;
	}
	if (!channel)
		return;
	if (channel->label() == kInputChannelLabel)
		m_pInputDataChannel->setChannel(channel);
	else if (channel->label() == kRealtimeInputChannelLabel)
		m_pRealtimeInputDataChannel->setChannel(channel);
	else if (channel->label() == kSessionChannelLabel)
		m_pSessionDataChannel->setChannel(channel);
	else if (channel->label() == kClipboardChannelLabel)
		m_pClipboardDataChannel->setChannel(channel);
	else if (channel->label() == kTerminalChannelLabel)
		m_pTerminalDataChannel->setChannel(channel);
	else if (channel->label() == kFileTransferControlChannelLabel)
	{
		if (m_role != ControlledSessionRole
			|| m_pFileTransferControlDataChannel->hasChannel())
		{
			channel->Close();
			return;
		}
		m_pFileTransferControlDataChannel->setChannel(channel);
	}
	else if (channel->label() == kFileTransferDataChannelLabel)
	{
		if (m_role != ControlledSessionRole
			|| m_pFileTransferDataDataChannel->hasChannel())
		{
			channel->Close();
			return;
		}
		m_pFileTransferDataDataChannel->setChannel(channel);
	}
}

void KWebRtcPeer::OnRenegotiationNeeded()
{
}

void KWebRtcPeer::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state)
{
	if (QThread::currentThread() != thread())
	{
		postCallback(m_nGeneration.load(),
			[new_state](KWebRtcPeer *pPeer)
			{
				pPeer->OnIceConnectionChange(new_state);
			});
		return;
	}
	emit stateChanged(m_nGeneration.load(), stringViewToQString(webrtc::PeerConnectionInterface::AsString(new_state)));
	if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted)
	{
		if (m_role == ControllerSessionRole)
			startStatsPolling(QStringLiteral("ice_connected"));
		emit connectionRestored(m_nGeneration.load());
	}
	else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected)
	{
		stopStatsPolling();
		emit connectionInterrupted(m_nGeneration.load());
	}
	else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed
		|| new_state == webrtc::PeerConnectionInterface::kIceConnectionClosed)
	{
		stopStatsPolling();
		emit connectionTerminated(m_nGeneration.load(),
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

	KWebRtcSignalingMessage message;
	message.type = IceCandidateWebRtcSignalingMessageType;
	message.strSdpMid = QString::fromStdString(pCandidate->sdp_mid());
	message.nSdpMLineIndex = pCandidate->sdp_mline_index();
	message.strCandidate = QString::fromStdString(pCandidate->ToString());
	const quint64 nGeneration = m_nGeneration.load();
	postCallback(nGeneration,
		[message, nGeneration](KWebRtcPeer *pPeer)
		{
			emit pPeer->signalingMessageReady(nGeneration,
				KWebRtcSignalingMessageCodec::encode(message));
		});
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
	emit inputChannelChanged(m_nGeneration.load(), bOpen);
	if (bOpen)
	{
		flushInputQueue();
		startStatsPolling(QStringLiteral("input_channel_open"));
	}
	else
	{
		m_inputSendQueue.clear();
	}
}

void KWebRtcPeer::handleRealtimeInputChannelChanged(bool bOpen)
{
	KSessionTraceLogger::write(roleToString(m_role),
		QStringLiteral("channel"), QStringLiteral("input-realtime"), -1,
		QStringLiteral("open=%1 enabled=%2")
			.arg(bOpen ? 1 : 0)
			.arg(m_bInputRealtimeEnabled ? 1 : 0));
	if (bOpen)
		flushRealtimeInputQueue();
	else
		m_realtimeInputSendQueue.clear();
}

void KWebRtcPeer::handleSessionChannelChanged(bool bOpen)
{
	emit sessionChannelChanged(m_nGeneration.load(), bOpen);
	if (bOpen)
		flushSessionQueue();
	else
		m_sessionSendQueue.clear();
	emit stateChanged(m_nGeneration.load(), bOpen
		? QStringLiteral("SessionChannelOpen")
		: QStringLiteral("SessionChannelClosed"));
}

void KWebRtcPeer::handleClipboardChannelChanged(bool bOpen)
{
	emit clipboardChannelChanged(m_nGeneration.load(), bOpen);
	if (bOpen)
		flushClipboardQueue();
	else
		m_clipboardSendQueue.clear();
}

void KWebRtcPeer::handleTerminalChannelChanged(bool bOpen)
{
	emit terminalChannelChanged(m_nGeneration.load(), bOpen);
}

void KWebRtcPeer::handleFileTransferChannelChanged()
{
	emit fileTransferChannelsChanged(m_nGeneration.load(),
		m_pFileTransferControlDataChannel->isOpen(),
		m_pFileTransferDataDataChannel->isOpen());
}

void KWebRtcPeer::handleSessionChannelMessage(const QString &strMessage)
{
	routeDataMessage(SessionProtocolChannel, strMessage, &m_nInvalidSessionMessages);
}

KProtocolHandlerResult KWebRtcPeer::decodeSessionMessage(const KProtocolEnvelope &envelope)
{
	KSessionMessage message;
	QString strError;
	if (!KSessionMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	emit sessionMessageReceived(m_nGeneration.load(), message);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KWebRtcPeer::decodeTerminalMessage(const KProtocolEnvelope &envelope)
{
	KTerminalMessage message;
	QString strError;
	if (!KTerminalMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	emit terminalControlMessageReceived(m_nGeneration.load(), message);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KWebRtcPeer::decodeFileTransferLifecycleMessage(
	const KProtocolEnvelope &envelope)
{
	KFileTransferLifecycleMessage message;
	QString strError;
	if (!KFileTransferLifecycleMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	emit fileTransferLifecycleMessageReceived(m_nGeneration.load(), message);
	return KProtocolHandlerResult::success();
}

KProtocolHandlerResult KWebRtcPeer::decodeFileTransferControlMessage(
	const KProtocolEnvelope &envelope)
{
	KFileTransferControlMessage message;
	QString strError;
	if (!KFileTransferControlMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	emit fileTransferControlMessageReceived(m_nGeneration.load(), message);
	return KProtocolHandlerResult::success();
}

void KWebRtcPeer::handleClipboardChannelMessage(const QString &strMessage)
{
	routeDataMessage(ClipboardProtocolChannel, strMessage, &m_nInvalidClipboardMessages);
}

KProtocolHandlerResult KWebRtcPeer::decodeClipboardMessage(const KProtocolEnvelope &envelope)
{
	KClipboardMessage message;
	QString strError;
	if (!KClipboardMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	emit clipboardMessageReceived(m_nGeneration.load(), message);
	return KProtocolHandlerResult::success();
}

void KWebRtcPeer::handleInputChannelMessage(const QString &strMessage)
{
	routeDataMessage(InputProtocolChannel, strMessage, &m_nInvalidInputMessages);
}

void KWebRtcPeer::handleRealtimeInputChannelMessage(const QString &strMessage)
{
	routeDataMessage(InputProtocolChannel, strMessage,
		&m_nInvalidRealtimeInputMessages, true);
}

KProtocolHandlerResult KWebRtcPeer::handleLatencyMessage(const KProtocolEnvelope &envelope)
{
	QString strResponse;
	if (m_spLatencyProbe->handleMessage(envelope, m_role, &strResponse))
	{
		if (!strResponse.isEmpty()
			&& (!m_pInputDataChannel->isOpen()
				|| !m_pInputDataChannel->sendText(strResponse)))
		{
			return KProtocolHandlerResult::failure(ProtocolHandlerExecutionFailed,
				QStringLiteral("Unable to send latency response"));
		}
		return KProtocolHandlerResult::success();
	}
	return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed,
		QStringLiteral("Invalid latency message"));
}

KProtocolHandlerResult KWebRtcPeer::decodeInputMessage(const KProtocolEnvelope &envelope,
	bool bRealtimeInput)
{
	KInputMessage message;
	QString strError;
	if (!KInputMessageCodec::decode(envelope, &message, &strError))
		return KProtocolHandlerResult::failure(ProtocolHandlerDecodeFailed, strError);
	if (bRealtimeInput && message.type != MouseMoveInputMessageType)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerPermissionDenied,
			QStringLiteral("Only mouse movement is allowed on the realtime input channel"));
	}
	if (bRealtimeInput && !m_bInputRealtimeEnabled)
	{
		return KProtocolHandlerResult::failure(ProtocolHandlerInvalidState,
			QStringLiteral("Realtime input was not negotiated"));
	}
	message.bRealtime = bRealtimeInput;
	emit inputMessageReceived(m_nGeneration.load(), message);
	return KProtocolHandlerResult::success();
}

void KWebRtcPeer::routeDataMessage(KProtocolChannel channel,
	const QString &strMessage,
	std::atomic_int *pInvalidCount,
	bool bRealtimeInput)
{
	KProtocolRouteContext context;
	context.nRole = static_cast<int>(m_role);
	context.bRealtimeInput = bRealtimeInput;
	const KProtocolRouteResult result = m_protocolRouter.route(channel, strMessage, context);
	KSessionTraceLogger::write(roleToString(m_role),
		QStringLiteral("protocol_route"),
		KProtocolEnvelopeCodec::channelName(channel),
		strMessage.toUtf8().size(),
		QStringLiteral("type=%1 routeStatus=%2 handlerStatus=%3")
			.arg(result.envelope.strType)
			.arg(static_cast<int>(result.status))
			.arg(static_cast<int>(result.handlerResult.status)));
	if (result.status == HandledProtocolRouteStatus)
	{
		if (pInvalidCount != nullptr)
			*pInvalidCount = 0;
		return;
	}
	const QString strDiagnosticChannel = bRealtimeInput
		? QStringLiteral("input-realtime")
		: KProtocolEnvelopeCodec::channelName(channel);
	handleProtocolReject(strDiagnosticChannel,
		strMessage.toUtf8().size(), result.strError, pInvalidCount);
}

void KWebRtcPeer::handleProtocolReject(const QString &strChannel,
	int nMessageBytes,
	const QString &strError,
	std::atomic_int *pInvalidCount)
{
	const int nInvalidCount = pInvalidCount != nullptr
		? pInvalidCount->fetch_add(1) + 1
		: 1;
	KSessionTraceLogger::write(roleToString(m_role),
		QStringLiteral("protocol_reject"),
		strChannel,
		nMessageBytes,
		QStringLiteral("error=%1 consecutive=%2")
			.arg(strError)
			.arg(nInvalidCount));
	if (nInvalidCount >= KProtocolConstraints::kMaximumInvalidMessages)
		terminateForProtocolViolation(strChannel);
}

void KWebRtcPeer::terminateForProtocolViolation(const QString &strChannel)
{
	if (m_bProtocolTerminationPending.exchange(true))
		return;

	const QPointer<KWebRtcPeer> pPeer(this);
	QMetaObject::invokeMethod(this,
		[pPeer, strChannel]()
		{
			if (!pPeer)
				return;
			emit pPeer->protocolViolation(pPeer->m_nGeneration.load(), strChannel, QStringLiteral(
				"Remote peer sent too many invalid %1 messages").arg(strChannel));
		},
		Qt::QueuedConnection);
}

void KWebRtcPeer::handleLocalDescription(webrtc::SessionDescriptionInterface *pDescription)
{
	if (m_spPeerConnection == nullptr || pDescription == nullptr)
		return;

	sendSessionDescription(pDescription);
	m_spPeerConnection->SetLocalDescription(KSetDescriptionObserver::create(
			m_spCallbackGate,
			m_nGeneration.load(),
			[](KWebRtcPeer *pPeer)
			{
				emit pPeer->stateChanged(pPeer->m_nGeneration.load(),
					QStringLiteral("LocalDescriptionSet"));
			},
			[](KWebRtcPeer *pPeer, webrtc::RTCError error)
			{
				pPeer->handleLocalDescriptionFailure(error);
			})
			.get(),
		pDescription);
}

void KWebRtcPeer::handleLocalDescriptionFailure(webrtc::RTCError error)
{
	emit transportError(m_nGeneration.load(), rtcErrorMessage(QStringLiteral("Create SDP failed"), error));
}

void KWebRtcPeer::handleRemoteDescriptionSuccess(webrtc::SdpType sdpType)
{
	emit stateChanged(m_nGeneration.load(), QStringLiteral("RemoteDescriptionSet"));
	if (sdpType == webrtc::SdpType::kOffer && m_spPeerConnection)
	{
		m_spPeerConnection->CreateAnswer(KCreateSessionDescriptionObserver::create(
			m_spCallbackGate, m_nGeneration.load()).get(),
			webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
	}
}

void KWebRtcPeer::handleRemoteDescriptionFailure(webrtc::RTCError error)
{
	emit transportError(m_nGeneration.load(), rtcErrorMessage(QStringLiteral("Set remote SDP failed"), error));
}

void KWebRtcPeer::OnFrame(const webrtc::VideoFrame &frame)
{
	m_pRemoteFrameProcessor->enqueue(frame);
}

bool KWebRtcPeer::createThreads(QString *pErrorMessage)
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

	if (!m_spNetworkThread->Start()
		|| !m_spWorkerThread->Start()
		|| !m_spSignalingThread->Start())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Start WebRTC threads failed");
		return false;
	}
	return true;
}

bool KWebRtcPeer::createFactory(QString *pErrorMessage)
{
	webrtc::PeerConnectionFactoryDependencies deps;
	deps.network_thread = m_spNetworkThread.get();
	deps.worker_thread = m_spWorkerThread.get();
	deps.signaling_thread = m_spSignalingThread.get();
	deps.env = webrtc::CreateEnvironment();
	deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
	deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
	deps.video_encoder_factory = std::make_unique<KWebRtcH264EncoderFactory>(
		m_encoderPreference);
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

bool KWebRtcPeer::createRealtimeInputDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = false;
	init.maxRetransmits = 0;
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>> result =
		m_spPeerConnection->CreateDataChannelOrError(kRealtimeInputChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = rtcErrorMessage(
				QStringLiteral("Create realtime input DataChannel failed"), result.error());
		}
		return false;
	}

	m_pRealtimeInputDataChannel->setChannel(result.value());
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

bool KWebRtcPeer::createClipboardDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = true;
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>> result =
		m_spPeerConnection->CreateDataChannelOrError(kClipboardChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = rtcErrorMessage(
				QStringLiteral("Create clipboard DataChannel failed"), result.error());
		}
		return false;
	}

	m_pClipboardDataChannel->setChannel(result.value());
	return true;
}

bool KWebRtcPeer::createTerminalDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = true;
	webrtc::RTCErrorOr<webrtc::scoped_refptr<webrtc::DataChannelInterface>> result =
		m_spPeerConnection->CreateDataChannelOrError(kTerminalChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = rtcErrorMessage(
				QStringLiteral("Create terminal DataChannel failed"), result.error());
		}
		return false;
	}
	m_pTerminalDataChannel->setChannel(result.value());
	return true;
}

bool KWebRtcPeer::createFileTransferControlDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = true;
	auto result = m_spPeerConnection->CreateDataChannelOrError(
		kFileTransferControlChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = rtcErrorMessage(
				QStringLiteral("Create file-control DataChannel failed"), result.error());
		}
		return false;
	}
	m_pFileTransferControlDataChannel->setChannel(result.value());
	return true;
}

bool KWebRtcPeer::createFileTransferDataDataChannel(QString *pErrorMessage)
{
	webrtc::DataChannelInit init;
	init.ordered = true;
	auto result = m_spPeerConnection->CreateDataChannelOrError(
		kFileTransferDataChannelLabel, &init);
	if (!result.ok())
	{
		if (pErrorMessage != nullptr)
		{
			*pErrorMessage = rtcErrorMessage(
				QStringLiteral("Create file-data DataChannel failed"), result.error());
		}
		return false;
	}
	m_pFileTransferDataDataChannel->setChannel(result.value());
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

	KWebRtcSignalingMessage message;
	message.type = pDescription->GetType() == webrtc::SdpType::kOffer
		? OfferWebRtcSignalingMessageType
		: AnswerWebRtcSignalingMessageType;
	message.strSdp = QString::fromStdString(strSdp);
	emit signalingMessageReady(m_nGeneration.load(), KWebRtcSignalingMessageCodec::encode(message));
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
		emit transportError(m_nGeneration.load(), QStringLiteral("Unknown SDP type"));
		return;
	}

	webrtc::SdpParseError parseError;
	std::unique_ptr<webrtc::SessionDescriptionInterface> spDescription =
		webrtc::CreateSessionDescription(sdpType.value(), strSdp.toStdString(), &parseError);
	if (!spDescription)
	{
		emit transportError(m_nGeneration.load(), QString::fromStdString(parseError.description));
		return;
	}

	const webrtc::SdpType value = sdpType.value();
	m_spPeerConnection->SetRemoteDescription(KSetDescriptionObserver::create(
			m_spCallbackGate,
			m_nGeneration.load(),
			[value](KWebRtcPeer *pPeer)
			{
				pPeer->handleRemoteDescriptionSuccess(value);
			},
			[](KWebRtcPeer *pPeer, webrtc::RTCError error)
			{
				pPeer->handleRemoteDescriptionFailure(error);
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
		emit transportError(m_nGeneration.load(), QString::fromStdString(parseError.description));
		return;
	}

	if (!m_spPeerConnection->AddIceCandidate(spCandidate.get()))
		emit transportError(m_nGeneration.load(), QStringLiteral("Add ICE candidate failed"));
}

QString KWebRtcPeer::rtcErrorMessage(const QString &strPrefix, const webrtc::RTCError &error)
{
	const absl::string_view strMessage = error.message();
	return QStringLiteral("%1: %2").arg(strPrefix, QString::fromUtf8(strMessage.data(), static_cast<int>(strMessage.size())));
}
