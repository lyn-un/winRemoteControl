#include "transport/webrtc/webrtcsessionservice.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "core/input/inputinjectorinterface.h"
#include "core/session/deviceinfoprovider.h"
#include "input/inputinjector.h"
#include "transport/webrtc/webrtcsignaling.h"

#include <QtCore/QTimer>

#include <utility>

namespace
{
	constexpr int kDisconnectGraceMs = 5000;

	static QString roleToString(KSessionRole role)
	{
		return KSessionStateMachine::roleName(role);
	}

	static bool shouldTraceInputMessage(const KInputMessage &message)
	{
		return message.bTrace
			|| (message.type == KeyInputMessageType && KLatencyTraceLogger::isEnabled());
	}

	static QString inputTraceExtra(const KInputMessage &message)
	{
		QString strExtra = QStringLiteral("seq=%1 type=%2")
			.arg(message.nSequence)
			.arg(KInputMessageCodec::typeName(message.type));
		if (message.type == KeyInputMessageType)
			strExtra += QStringLiteral(" pressed=%1").arg(message.bPressed ? 1 : 0);
		return strExtra;
	}
}

KWebRtcSessionService::KWebRtcSessionService(
	std::unique_ptr<IKDeviceInfoProvider> spDeviceInfoProvider,
	std::unique_ptr<IKInputInjector> spInputInjector,
	QObject *pParent)
	: QObject(pParent)
	, m_spDeviceInfoProvider(std::move(spDeviceInfoProvider))
	, m_pSignaling(new KWebRtcSignaling(this))
	, m_pInputInjector(new KInputInjector(std::move(spInputInjector), this))
	, m_pDisconnectGraceTimer(new QTimer(this))
{
	m_pDisconnectGraceTimer->setSingleShot(true);
	connect(m_pDisconnectGraceTimer, &QTimer::timeout,
		this, &KWebRtcSessionService::handleDisconnectGraceTimeout);
	connect(m_pSignaling, &KWebRtcSignaling::stateChanged,
		this, &KWebRtcSessionService::signalingChanged);
	connect(m_pSignaling, &KWebRtcSignaling::signalingError,
		this, &KWebRtcSessionService::sessionError);
	connect(m_pSignaling, &KWebRtcSignaling::outgoingConnectionEstablished,
		this, &KWebRtcSessionService::handleOutgoingConnectionEstablished);
	connect(m_pSignaling, &KWebRtcSignaling::outgoingConnectionFailed,
		this, &KWebRtcSessionService::handleOutgoingConnectionFailed);
	connect(m_pSignaling, &KWebRtcSignaling::incomingConnectionEstablished,
		this, &KWebRtcSessionService::handleIncomingConnectionEstablished);
	connect(m_pSignaling, &KWebRtcSignaling::connectionLost,
		this, &KWebRtcSessionService::handleSignalingConnectionLost);
	connect(m_pInputInjector, &KInputInjector::inputError,
		this, &KWebRtcSessionService::sessionError);
}

KWebRtcSessionService::~KWebRtcSessionService()
{
	disconnectSession();
}

void KWebRtcSessionService::setRole(const QString &strRole)
{
	KSessionRole role;
	if (!KSessionStateMachine::roleFromString(strRole, &role))
		return;
	if (role != m_sessionStateMachine.role()
		&& m_sessionStateMachine.state() != IdleSessionState)
	{
		finishSession(RoleChangedSessionEndReason, QString(), false, true, false);
	}
	if (m_sessionStateMachine.state() == IdleSessionState)
		m_sessionStateMachine.setRole(role);

	emit webRtcStateChanged(QStringLiteral("Role:%1")
		.arg(KSessionStateMachine::roleName(m_sessionStateMachine.role())));
}

void KWebRtcSessionService::startSignalingServer(quint16 nPort)
{
	if (m_sessionStateMachine.state() != IdleSessionState)
		finishSession(RestartListenerSessionEndReason, QString(), false, true, false);
	QString strError;
	if (!initializePeer(KWebRtcPeer::ControlledRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	if (!m_pSignaling->startServer(nPort, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_sessionStateMachine.beginListening();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KWebRtcSessionService::connectSignaling(const QString &strHost, quint16 nPort)
{
	if (m_sessionStateMachine.state() != IdleSessionState)
		finishSession(NewConnectionSessionEndReason, QString(), false, true, false);
	m_pSignaling->stop();
	QString strError;
	if (!initializePeer(KWebRtcPeer::ControllerRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_sessionStateMachine.beginConnecting();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_pSignaling->connectToHost(strHost, nPort);
}

void KWebRtcSessionService::disconnectSession()
{
	finishSession(LocalDisconnectSessionEndReason, QString(), false, true, false);
}

void KWebRtcSessionService::enterRemoteDesktop(const KStreamConfig &config)
{
	if (!m_sessionStateMachine.canEnterRemoteDesktop() || !m_bSessionChannelOpen)
	{
		return;
	}

	KSessionTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("startup_order"),
		QStringLiteral("streamConfigBeforeStart"),
		-1,
		QStringLiteral("width=%1 height=%2 fps=%3 bitrateKbps=%4")
			.arg(config.nWidth)
			.arg(config.nHeight)
			.arg(config.nFps)
			.arg(config.nBitrateKbps));
	sendStreamConfig(config);
	KSessionMessage message;
	message.type = StartStreamingSessionMessageType;
	sendSessionMessage(message);
	m_sessionStateMachine.beginStreaming();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KWebRtcSessionService::leaveRemoteDesktop()
{
	if (!m_sessionStateMachine.canLeaveRemoteDesktop())
		return;
	KSessionMessage message;
	message.type = StopStreamingSessionMessageType;
	sendSessionMessage(message);
	m_sessionStateMachine.stopStreaming();
	emit stopCaptureRequested();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KWebRtcSessionService::startStreaming()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole)
	{
		emit sessionError(QStringLiteral("只有被控端可以开始推流"));
		return;
	}
	if (!m_bSessionChannelOpen || !m_sessionStateMachine.canStartControlledStreaming())
		return;

	emit startCaptureRequested();
	m_sessionStateMachine.beginStreaming();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KWebRtcSessionService::stopStreaming()
{
	if (m_sessionStateMachine.role() == ControlledSessionRole)
	{
		finishSession(ControlledUserStopSessionEndReason, QString(), true, true, false);
		return;
	}

	leaveRemoteDesktop();
}

void KWebRtcSessionService::pushVideoFrame(const KWebRtcVideoFrame &frame)
{
	if (m_pPeer != nullptr && m_sessionStateMachine.canSendVideo())
		m_pPeer->pushVideoFrame(frame);
}

void KWebRtcSessionService::sendInputMessage(const KInputMessage &message)
{
	if (!m_sessionStateMachine.canSendInput() || !m_bInputChannelOpen)
	{
		return;
	}
	if (m_pPeer == nullptr)
		return;

	const QString strMessage = KInputMessageCodec::encode(message);
	if (shouldTraceInputMessage(message))
	{
		KLatencyTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("input_send"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}
	m_pPeer->sendInputMessage(strMessage);
}

void KWebRtcSessionService::sendSessionMessage(const KSessionMessage &message)
{
	const QString strType = KSessionMessageCodec::typeName(message.type);
	const QString strMessage = KSessionMessageCodec::encode(message);
	if (m_pPeer == nullptr || !m_bSessionChannelOpen)
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("send_drop"),
			strType,
			strMessage.toUtf8().size(),
			QStringLiteral("reason=session_channel_not_open"));
		return;
	}

	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("send"),
		strType,
		strMessage.toUtf8().size());
	m_pPeer->sendSessionMessage(strMessage);
}

void KWebRtcSessionService::sendStreamConfig(const KStreamConfig &config)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	KSessionMessage message;
	message.type = StreamConfigSessionMessageType;
	message.streamConfig = config;
	sendSessionMessage(message);
}

void KWebRtcSessionService::handleCaptureFailure()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.state() != StreamingSessionState)
	{
		return;
	}

	finishSession(CaptureFailedSessionEndReason, QString(), true, true, true);
}

void KWebRtcSessionService::finishSession(KSessionEndReason reason,
	const QString &strDetail,
	bool bKeepListening,
	bool bNotifyRemote,
	bool bReportError)
{
	if (!m_sessionStateMachine.beginStopping())
		return;

	const QString strReason = KSessionStateMachine::endReasonName(reason, strDetail);
	const KSessionRole role = m_sessionStateMachine.role();
	const quint64 nGeneration = m_sessionStateMachine.generation();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	KSessionTraceLogger::write(roleToString(role),
		QStringLiteral("session_end"),
		strReason,
		-1,
		QStringLiteral("generation=%1 keepListening=%2 notifyRemote=%3")
			.arg(nGeneration)
			.arg(bKeepListening ? 1 : 0)
			.arg(bNotifyRemote ? 1 : 0));

	if (bNotifyRemote && m_bSessionChannelOpen)
	{
		KSessionMessage message;
		message.type = EndSessionMessageType;
		message.strReason = strReason;
		sendSessionMessage(message);
	}

	m_pDisconnectGraceTimer->stop();
	m_nDisconnectGraceGeneration = 0;
	m_bDeviceInfoRequested = false;
	m_bInputChannelOpen = false;
	m_bSessionChannelOpen = false;
	m_pInputInjector->releaseAllInputs();
	resetInputTraceState();
	emit stopCaptureRequested();
	emit sessionChannelChanged(false);
	emit networkStatsReady(KWebRtcNetworkStats());

	if (m_pPeer != nullptr)
		m_pPeer->shutdown();

	bool bListening = false;
	if (bKeepListening && role == ControlledSessionRole)
	{
		m_pSignaling->disconnectPeer();
		QString strError;
		if (initializePeer(KWebRtcPeer::ControlledRole, &strError))
		{
			bListening = true;
			m_sessionStateMachine.finish(true);
			emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		}
		else
		{
			m_pSignaling->stop();
			m_sessionStateMachine.finish(false);
			emit webRtcStateChanged(QStringLiteral("Failed"));
			emit sessionError(strError);
		}
	}
	else
	{
		m_pSignaling->stop();
		m_sessionStateMachine.finish(false);
		emit webRtcStateChanged(QStringLiteral("Disconnected"));
	}

	if (bReportError)
	{
		emit sessionError(QStringLiteral("Remote session ended: %1").arg(strReason));
	}
	else if (bListening)
	{
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("session_ready"),
			QStringLiteral("listening_after_end"));
	}
}

void KWebRtcSessionService::resetInputTraceState()
{
	m_nLastInjectedInputSeq = 0;
	m_nLastInjectedInputMs = -1;
	emit inputTraceUpdated(0, -1);
}

bool KWebRtcSessionService::initializePeer(KWebRtcPeer::Role role, QString *pErrorMessage)
{
	m_bDeviceInfoRequested = false;
	if (m_pPeer == nullptr)
	{
		m_pPeer = new KWebRtcPeer(this);
		wirePeer();
	}

	m_pPeer->shutdown();
	return m_pPeer->initialize(role, pErrorMessage);
}

void KWebRtcSessionService::wirePeer()
{
	connect(m_pPeer, &KWebRtcPeer::signalingMessageReady,
		m_pSignaling, &KWebRtcSignaling::sendJsonMessage);
	connect(m_pSignaling, &KWebRtcSignaling::messageReceived,
		m_pPeer, &KWebRtcPeer::handleSignalingMessage);
	connect(m_pPeer, &KWebRtcPeer::stateChanged,
		this, &KWebRtcSessionService::webRtcStateChanged);
	connect(m_pPeer, &KWebRtcPeer::peerError,
		this, &KWebRtcSessionService::sessionError);
	connect(m_pPeer, &KWebRtcPeer::remoteFrameReady,
		this, &KWebRtcSessionService::handleRemoteFrame, Qt::DirectConnection);
	connect(m_pPeer, &KWebRtcPeer::networkStatsReady,
		this, &KWebRtcSessionService::networkStatsReady);
	connect(m_pPeer, &KWebRtcPeer::inputMessageReceived,
		this, &KWebRtcSessionService::handleInputMessage);
	connect(m_pInputInjector, &KInputInjector::inputInjected,
		this, &KWebRtcSessionService::handleInputInjected);
	connect(m_pPeer, &KWebRtcPeer::inputChannelChanged,
		this, &KWebRtcSessionService::handleInputChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::sessionMessageReceived,
		this, &KWebRtcSessionService::handleSessionMessage);
	connect(m_pPeer, &KWebRtcPeer::sessionChannelChanged,
		this, &KWebRtcSessionService::handleSessionChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::sessionChannelChanged,
		this, &KWebRtcSessionService::sessionChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::peerConnectionInterrupted,
		this, &KWebRtcSessionService::handlePeerConnectionInterrupted);
	connect(m_pPeer, &KWebRtcPeer::peerConnectionRestored,
		this, &KWebRtcSessionService::handlePeerConnectionRestored);
	connect(m_pPeer, &KWebRtcPeer::peerConnectionTerminated,
		this, &KWebRtcSessionService::handlePeerConnectionTerminated);
}

void KWebRtcSessionService::handleRemoteFrame(const KDecodedVideoFrame &frame)
{
	// The peer already coalesces remote frames (drop-old) and the render widget
	// coalesces again before present, so this middle layer only forwards the
	// frame instead of adding another mutex+QueuedConnection hop.
	if (frame.nWidth <= 0 || frame.nHeight <= 0 || frame.vecBgraBuffer.empty())
		return;

	emit remoteFrameReady(frame);
	emit remoteFrameStatsReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
}

void KWebRtcSessionService::handleInputMessage(const QString &strMessage)
{
	KInputMessage message;
	QString strError;
	if (!KInputMessageCodec::decode(strMessage, &message, &strError))
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("protocol_reject"),
			QStringLiteral("input"),
			strMessage.toUtf8().size(),
			strError);
		return;
	}

	if (shouldTraceInputMessage(message))
	{
		KLatencyTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("input_recv"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}

	if (!m_sessionStateMachine.canReceiveInput())
		return;

	m_pInputInjector->handleInputMessage(message);
}

void KWebRtcSessionService::handleInputChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bInputChannelOpen;
	m_bInputChannelOpen = bOpen;
	if (!bOpen)
		m_pInputInjector->releaseAllInputs();
	emit inputChannelChanged(bOpen);
	if (bWasOpen && !bOpen && !m_sessionStateMachine.isStopping())
	{
		finishSession(InputChannelClosedSessionEndReason,
			QString(),
			m_sessionStateMachine.shouldKeepListening(),
			false,
			true);
	}
}

void KWebRtcSessionService::handleSessionChannelChanged(bool bOpen)
{
	const bool bWasOpen = m_bSessionChannelOpen;
	m_bSessionChannelOpen = bOpen;
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("channel"),
		QStringLiteral("session"),
		-1,
		QStringLiteral("open=%1").arg(bOpen ? 1 : 0));
	if (!bOpen)
	{
		m_bDeviceInfoRequested = false;
		if (bWasOpen && !m_sessionStateMachine.isStopping())
		{
			finishSession(SessionChannelClosedSessionEndReason,
				QString(),
				m_sessionStateMachine.shouldKeepListening(),
				false,
				true);
		}
		return;
	}
	if (!m_sessionStateMachine.markConnected())
		return;
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	if (m_sessionStateMachine.role() == ControllerSessionRole)
	{
		if (m_bDeviceInfoRequested)
		{
			KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
				QStringLiteral("skip"),
				KSessionMessageCodec::typeName(DeviceInfoRequestSessionMessageType),
				-1,
				QStringLiteral("reason=already_requested"));
			return;
		}

		m_bDeviceInfoRequested = true;
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("handle_channel_open"),
			KSessionMessageCodec::typeName(DeviceInfoRequestSessionMessageType));
		KSessionMessage message;
		message.type = DeviceInfoRequestSessionMessageType;
		sendSessionMessage(message);
	}
}

void KWebRtcSessionService::handleSessionMessage(const QString &strMessage)
{
	KSessionMessage message;
	QString strError;
	if (!KSessionMessageCodec::decode(strMessage, &message, &strError))
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("protocol_reject"),
			QStringLiteral("session"),
			strMessage.toUtf8().size(),
			strError);
		return;
	}

	const QString strType = KSessionMessageCodec::typeName(message.type);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("handle"),
		strType,
		strMessage.toUtf8().size());
	if (message.type == DeviceInfoRequestSessionMessageType
		&& m_sessionStateMachine.role() == ControlledSessionRole)
	{
		sendDeviceInfoMessage();
		return;
	}

	if (message.type == DeviceInfoSessionMessageType
		&& m_sessionStateMachine.role() == ControllerSessionRole)
	{
		emit remoteDeviceInfoChanged(message.deviceInfo.strComputerName,
			message.deviceInfo.strWallpaperMime,
			message.deviceInfo.strWallpaperData,
			message.deviceInfo.nScreenWidth,
			message.deviceInfo.nScreenHeight);
		return;
	}

	if (message.type == StartStreamingSessionMessageType
		&& m_sessionStateMachine.canStartControlledStreaming())
	{
		KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("emit"),
			QStringLiteral("startCaptureRequested"));
		emit startCaptureRequested();
		m_sessionStateMachine.beginStreaming();
		emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		return;
	}

	if (message.type == StopStreamingSessionMessageType
		&& m_sessionStateMachine.role() == ControlledSessionRole
		&& m_sessionStateMachine.state() == StreamingSessionState)
	{
		m_sessionStateMachine.stopStreaming();
		m_pInputInjector->releaseAllInputs();
		resetInputTraceState();
		emit stopCaptureRequested();
		emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
		return;
	}

	if (message.type == EndSessionMessageType)
	{
		const QString strRemoteReason = message.strReason;
		finishSession(RemoteStopSessionEndReason,
			strRemoteReason,
			m_sessionStateMachine.shouldKeepListening(),
			false,
			m_sessionStateMachine.role() == ControllerSessionRole);
		return;
	}

	if (message.type == StreamConfigSessionMessageType
		&& m_sessionStateMachine.role() == ControlledSessionRole)
	{
		if (m_pPeer != nullptr)
			m_pPeer->setStreamConfig(message.streamConfig);
		emit streamConfigChanged(message.streamConfig);
	}
}

void KWebRtcSessionService::handleInputInjected(quint64 nSeq, qint64 nInjectedMs)
{
	m_nLastInjectedInputSeq = nSeq;
	m_nLastInjectedInputMs = nInjectedMs;
	emit inputTraceUpdated(nSeq, nInjectedMs);
	emit inputFeedbackFrameRequested();
}

void KWebRtcSessionService::handleOutgoingConnectionEstablished()
{
	if (!m_sessionStateMachine.isConnecting() || m_pPeer == nullptr)
		return;

	m_sessionStateMachine.beginNegotiating();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_pPeer->createOffer();
}

void KWebRtcSessionService::handleOutgoingConnectionFailed(const QString &strMessage)
{
	if (!m_sessionStateMachine.isConnecting())
		return;

	finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
	emit sessionError(strMessage);
}

void KWebRtcSessionService::handleIncomingConnectionEstablished()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.isStopping())
		return;

	if (!m_sessionStateMachine.beginNegotiating())
		return;
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KWebRtcSessionService::handleSignalingConnectionLost()
{
	if (m_sessionStateMachine.isNegotiating())
	{
		finishSession(SignalingLostSessionEndReason,
			QString(),
			m_sessionStateMachine.shouldKeepListening(),
			false,
			true);
	}
}

void KWebRtcSessionService::handlePeerConnectionInterrupted()
{
	if (!m_sessionStateMachine.interrupt())
		return;

	m_pInputInjector->releaseAllInputs();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_nDisconnectGraceGeneration = m_sessionStateMachine.generation();
	m_pDisconnectGraceTimer->start(kDisconnectGraceMs);
}

void KWebRtcSessionService::handlePeerConnectionRestored()
{
	if (!m_sessionStateMachine.restore())
		return;

	m_pDisconnectGraceTimer->stop();
	m_nDisconnectGraceGeneration = 0;
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KWebRtcSessionService::handlePeerConnectionTerminated(const QString &strReason)
{
	if (!m_sessionStateMachine.canHandlePeerTermination())
		return;

	finishSession(PeerTerminatedSessionEndReason,
		strReason,
		m_sessionStateMachine.shouldKeepListening(),
		false,
		true);
}

void KWebRtcSessionService::handleDisconnectGraceTimeout()
{
	if (!m_sessionStateMachine.isInterrupted()
		|| m_nDisconnectGraceGeneration != m_sessionStateMachine.generation())
		return;

	finishSession(DisconnectTimeoutSessionEndReason,
		QString(),
		m_sessionStateMachine.shouldKeepListening(),
		false,
		true);
}

void KWebRtcSessionService::sendDeviceInfoMessage()
{
	KSessionMessage message;
	message.type = DeviceInfoSessionMessageType;
	if (m_spDeviceInfoProvider != nullptr)
		message.deviceInfo = m_spDeviceInfoProvider->deviceInfo();
	const QString strMessage = KSessionMessageCodec::encode(message);
	KSessionTraceLogger::write(roleToString(m_sessionStateMachine.role()),
		QStringLiteral("prepare"),
		KSessionMessageCodec::typeName(DeviceInfoSessionMessageType),
		strMessage.toUtf8().size(),
		QStringLiteral("wallpaper=%1 wallpaperBytes=%2 messageBytes=%3")
			.arg(message.deviceInfo.strWallpaperData.isEmpty() ? 0 : 1)
			.arg(message.deviceInfo.strWallpaperData.size())
			.arg(strMessage.toUtf8().size()));
	sendSessionMessage(message);
}
