#include "session/sessioncoordinator.h"

#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "core/input/inputinjectorinterface.h"
#include "core/session/deviceinfoprovider.h"
#include "core/transport/signalingtransport.h"
#include "input/inputinjector.h"

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

KSessionCoordinator::KSessionCoordinator(
	std::unique_ptr<IKDeviceInfoProvider> spDeviceInfoProvider,
	std::unique_ptr<IKInputInjector> spInputInjector,
	std::unique_ptr<KRemotePeerTransport> spRemotePeerTransport,
	std::unique_ptr<KSignalingTransport> spSignalingTransport,
	QObject *pParent)
	: KSessionController(pParent)
	, m_spDeviceInfoProvider(std::move(spDeviceInfoProvider))
	, m_spRemotePeerTransport(std::move(spRemotePeerTransport))
	, m_spSignalingTransport(std::move(spSignalingTransport))
	, m_pSignaling(m_spSignalingTransport.get())
	, m_pInputInjector(new KInputInjector(std::move(spInputInjector), this))
	, m_pDisconnectGraceTimer(new QTimer(this))
{
	Q_ASSERT(m_spRemotePeerTransport != nullptr);
	Q_ASSERT(m_pSignaling != nullptr);
	m_pDisconnectGraceTimer->setSingleShot(true);
	connect(m_pDisconnectGraceTimer, &QTimer::timeout,
		this, &KSessionCoordinator::handleDisconnectGraceTimeout);
	connect(m_pSignaling, &KSignalingTransport::stateChanged,
		this, &KSessionCoordinator::signalingChanged);
	connect(m_pSignaling, &KSignalingTransport::signalingError,
		this, &KSessionCoordinator::sessionError);
	connect(m_pSignaling, &KSignalingTransport::outgoingConnectionEstablished,
		this, &KSessionCoordinator::handleOutgoingConnectionEstablished);
	connect(m_pSignaling, &KSignalingTransport::outgoingConnectionFailed,
		this, &KSessionCoordinator::handleOutgoingConnectionFailed);
	connect(m_pSignaling, &KSignalingTransport::incomingConnectionEstablished,
		this, &KSessionCoordinator::handleIncomingConnectionEstablished);
	connect(m_pSignaling, &KSignalingTransport::connectionLost,
		this, &KSessionCoordinator::handleSignalingConnectionLost);
	connect(m_pInputInjector, &KInputInjector::inputError,
		this, &KSessionCoordinator::sessionError);
	wirePeer();
}

KSessionCoordinator::~KSessionCoordinator()
{
	disconnectSession();
}

void KSessionCoordinator::setRole(const QString &strRole)
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

void KSessionCoordinator::startSignalingServer(quint16 nPort)
{
	if (m_sessionStateMachine.state() != IdleSessionState)
		finishSession(RestartListenerSessionEndReason, QString(), false, true, false);
	QString strError;
	if (!initializePeer(ControlledSessionRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	if (!m_pSignaling->startServer(nPort, &strError))
	{
		updateListeningAvailability(false);
		emit sessionError(strError);
		return;
	}

	m_sessionStateMachine.beginListening();
	updateListeningAvailability(true, nPort);
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::connectSignaling(const QString &strHost, quint16 nPort)
{
	if (m_sessionStateMachine.state() != IdleSessionState)
		finishSession(NewConnectionSessionEndReason, QString(), false, true, false);
	m_pSignaling->stop();
	QString strError;
	if (!initializePeer(ControllerSessionRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_sessionStateMachine.beginConnecting();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_pSignaling->connectToHost(strHost, nPort);
}

void KSessionCoordinator::disconnectSession()
{
	finishSession(LocalDisconnectSessionEndReason, QString(), false, true, false);
}

void KSessionCoordinator::enterRemoteDesktop(const KStreamConfig &config)
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

void KSessionCoordinator::leaveRemoteDesktop()
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

void KSessionCoordinator::startStreaming()
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

void KSessionCoordinator::stopStreaming()
{
	if (m_sessionStateMachine.role() == ControlledSessionRole)
	{
		if (!m_sessionStateMachine.hasActiveSession())
			return;
		finishSession(ControlledUserStopSessionEndReason, QString(), true, true, false);
		return;
	}

	leaveRemoteDesktop();
}

void KSessionCoordinator::pushVideoFrame(const KVideoFrame &frame)
{
	if (m_sessionStateMachine.canSendVideo())
		m_spRemotePeerTransport->pushVideoFrame(frame);
}

void KSessionCoordinator::sendInputMessage(const KInputMessage &message)
{
	if (!m_sessionStateMachine.canSendInput() || !m_bInputChannelOpen)
	{
		return;
	}
	const QString strMessage = KInputMessageCodec::encode(message);
	if (shouldTraceInputMessage(message))
	{
		KLatencyTraceLogger::write(roleToString(m_sessionStateMachine.role()),
			QStringLiteral("input_send"),
			QStringLiteral("%1 size=%2")
				.arg(inputTraceExtra(message))
				.arg(strMessage.toUtf8().size()));
	}
	m_spRemotePeerTransport->sendInputMessage(message);
}

void KSessionCoordinator::sendSessionMessage(const KSessionMessage &message)
{
	const QString strType = KSessionMessageCodec::typeName(message.type);
	const QString strMessage = KSessionMessageCodec::encode(message);
	if (!m_bSessionChannelOpen)
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
	m_spRemotePeerTransport->sendSessionMessage(message);
}

void KSessionCoordinator::sendStreamConfig(const KStreamConfig &config)
{
	if (m_sessionStateMachine.role() != ControllerSessionRole)
		return;
	KSessionMessage message;
	message.type = StreamConfigSessionMessageType;
	message.streamConfig = config;
	sendSessionMessage(message);
}

void KSessionCoordinator::handleCaptureFailure()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.state() != StreamingSessionState)
	{
		return;
	}

	finishSession(CaptureFailedSessionEndReason, QString(), true, true, true);
}

void KSessionCoordinator::finishSession(KSessionEndReason reason,
	const QString &strDetail,
	bool bKeepListening,
	bool bNotifyRemote,
	bool bReportError)
{
	if (!m_sessionStateMachine.beginStopping())
		return;
	updateListeningAvailability(false);

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
	emit networkStatsReady(KNetworkStats());
	m_spRemotePeerTransport->shutdown();

	bool bListening = false;
	if (bKeepListening && role == ControlledSessionRole)
	{
		m_pSignaling->disconnectPeer();
		QString strError;
		if (initializePeer(ControlledSessionRole, &strError))
		{
			bListening = true;
			m_sessionStateMachine.finish(true);
			updateListeningAvailability(true, m_nListeningPort);
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

void KSessionCoordinator::resetInputTraceState()
{
	m_nLastInjectedInputSeq = 0;
	m_nLastInjectedInputMs = -1;
	emit inputTraceUpdated(0, -1);
}

bool KSessionCoordinator::initializePeer(KSessionRole role, QString *pErrorMessage)
{
	m_bDeviceInfoRequested = false;
	m_spRemotePeerTransport->shutdown();
	return m_spRemotePeerTransport->initialize(role, pErrorMessage);
}

void KSessionCoordinator::wirePeer()
{
	KRemotePeerTransport *pTransport = m_spRemotePeerTransport.get();
	connect(pTransport, &KRemotePeerTransport::signalingMessageReady,
		m_pSignaling, &KSignalingTransport::sendMessage);
	connect(m_pSignaling, &KSignalingTransport::messageReceived,
		pTransport, &KRemotePeerTransport::handleSignalingMessage);
	connect(pTransport, &KRemotePeerTransport::stateChanged,
		this, &KSessionCoordinator::webRtcStateChanged);
	connect(pTransport, &KRemotePeerTransport::transportError,
		this, &KSessionCoordinator::sessionError);
	connect(pTransport, &KRemotePeerTransport::remoteFrameReady,
		this, &KSessionCoordinator::handleRemoteFrame, Qt::DirectConnection);
	connect(pTransport, &KRemotePeerTransport::networkStatsReady,
		this, &KSessionCoordinator::networkStatsReady);
	connect(pTransport, &KRemotePeerTransport::inputMessageReceived,
		this, &KSessionCoordinator::handleInputMessage);
	connect(m_pInputInjector, &KInputInjector::inputInjected,
		this, &KSessionCoordinator::handleInputInjected);
	connect(pTransport, &KRemotePeerTransport::inputChannelChanged,
		this, &KSessionCoordinator::handleInputChannelChanged);
	connect(pTransport, &KRemotePeerTransport::sessionMessageReceived,
		this, &KSessionCoordinator::handleSessionMessage);
	connect(pTransport, &KRemotePeerTransport::sessionChannelChanged,
		this, &KSessionCoordinator::handleSessionChannelChanged);
	connect(pTransport, &KRemotePeerTransport::sessionChannelChanged,
		this, &KSessionCoordinator::sessionChannelChanged);
	connect(pTransport, &KRemotePeerTransport::connectionInterrupted,
		this, &KSessionCoordinator::handlePeerConnectionInterrupted);
	connect(pTransport, &KRemotePeerTransport::connectionRestored,
		this, &KSessionCoordinator::handlePeerConnectionRestored);
	connect(pTransport, &KRemotePeerTransport::connectionTerminated,
		this, &KSessionCoordinator::handlePeerConnectionTerminated);
}

void KSessionCoordinator::handleRemoteFrame(const KDecodedVideoFrame &frame)
{
	// The peer already coalesces remote frames (drop-old) and the render widget
	// coalesces again before present, so this middle layer only forwards the
	// frame instead of adding another mutex+QueuedConnection hop.
	if (frame.nWidth <= 0 || frame.nHeight <= 0 || frame.vecBgraBuffer.empty())
		return;

	emit remoteFrameReady(frame);
	emit remoteFrameStatsReady(frame.nWidth, frame.nHeight, frame.nFrameIndex, frame.nTimestampMs);
}

void KSessionCoordinator::handleInputMessage(const KInputMessage &message)
{
	if (shouldTraceInputMessage(message))
	{
		const QString strMessage = KInputMessageCodec::encode(message);
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

void KSessionCoordinator::handleInputChannelChanged(bool bOpen)
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

void KSessionCoordinator::handleSessionChannelChanged(bool bOpen)
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

void KSessionCoordinator::handleSessionMessage(const KSessionMessage &message)
{
	const QString strMessage = KSessionMessageCodec::encode(message);
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
		m_spRemotePeerTransport->setStreamConfig(message.streamConfig);
		emit streamConfigChanged(message.streamConfig);
	}
}

void KSessionCoordinator::handleInputInjected(quint64 nSeq, qint64 nInjectedMs)
{
	m_nLastInjectedInputSeq = nSeq;
	m_nLastInjectedInputMs = nInjectedMs;
	emit inputTraceUpdated(nSeq, nInjectedMs);
	emit inputFeedbackFrameRequested();
}

void KSessionCoordinator::handleOutgoingConnectionEstablished()
{
	if (!m_sessionStateMachine.isConnecting())
		return;

	m_sessionStateMachine.beginNegotiating();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_spRemotePeerTransport->createOffer();
}

void KSessionCoordinator::handleOutgoingConnectionFailed(const QString &strMessage)
{
	if (!m_sessionStateMachine.isConnecting())
		return;

	finishSession(ConnectFailedSessionEndReason, QString(), false, false, false);
	emit sessionError(strMessage);
}

void KSessionCoordinator::handleIncomingConnectionEstablished()
{
	if (m_sessionStateMachine.role() != ControlledSessionRole
		|| m_sessionStateMachine.isStopping())
		return;

	if (!m_sessionStateMachine.beginNegotiating())
		return;
	updateListeningAvailability(false);
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::updateListeningAvailability(bool bAvailable, quint16 nPort)
{
	if (bAvailable && nPort != 0)
		m_nListeningPort = nPort;
	if (m_bListeningAvailable == bAvailable)
		return;
	m_bListeningAvailable = bAvailable;
	emit listeningAvailabilityChanged(bAvailable, bAvailable ? m_nListeningPort : 0);
}

void KSessionCoordinator::handleSignalingConnectionLost()
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

void KSessionCoordinator::handlePeerConnectionInterrupted()
{
	if (!m_sessionStateMachine.interrupt())
		return;

	m_pInputInjector->releaseAllInputs();
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
	m_nDisconnectGraceGeneration = m_sessionStateMachine.generation();
	m_pDisconnectGraceTimer->start(kDisconnectGraceMs);
}

void KSessionCoordinator::handlePeerConnectionRestored()
{
	if (!m_sessionStateMachine.restore())
		return;

	m_pDisconnectGraceTimer->stop();
	m_nDisconnectGraceGeneration = 0;
	emit webRtcStateChanged(KSessionStateMachine::stateName(m_sessionStateMachine.state()));
}

void KSessionCoordinator::handlePeerConnectionTerminated(const QString &strReason)
{
	if (!m_sessionStateMachine.canHandlePeerTermination())
		return;

	finishSession(PeerTerminatedSessionEndReason,
		strReason,
		m_sessionStateMachine.shouldKeepListening(),
		false,
		true);
}

void KSessionCoordinator::handleDisconnectGraceTimeout()
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

void KSessionCoordinator::sendDeviceInfoMessage()
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
