#include "session/sessionviewmodel.h"

#include "common/latencytracelogger.h"
#include "core/media/capturecontroller.h"
#include "core/protocol/protocolconstraints.h"
#include "session/sessioncontroller.h"

namespace
{
	constexpr int kRemoteMouseButtonLeft = 1;
	constexpr int kRemoteMouseButtonRight = 2;
	constexpr int kRemoteMouseButtonMiddle = 3;
	constexpr int kRemoteMouseButtonX1 = 4;
	constexpr int kRemoteMouseButtonX2 = 5;
	bool isSameStreamConfig(const KStreamConfig &lhs, const KStreamConfig &rhs)
	{
		return lhs.nFps == rhs.nFps
			&& lhs.nWidth == rhs.nWidth
			&& lhs.nHeight == rhs.nHeight
			&& lhs.nBitrateKbps == rhs.nBitrateKbps;
	}
}

KSessionViewModel::KSessionViewModel(KCaptureController *pCaptureController,
	KSessionController *pSessionController,
	QObject *pParent)
	: QObject(pParent)
	, m_pCaptureController(pCaptureController)
	, m_pSessionController(pSessionController)
{
	Q_ASSERT(m_pCaptureController != nullptr);
	Q_ASSERT(m_pSessionController != nullptr);
	initConnections();
}

KSessionViewModel::~KSessionViewModel()
{
}

QSize KSessionViewModel::remoteScreenSize() const
{
	return m_remoteScreenSize;
}

void KSessionViewModel::startLocalPreview()
{
	m_pCaptureController->startCapture();
}

void KSessionViewModel::stopCapture()
{
	m_pCaptureController->stopCapture();
}

void KSessionViewModel::setRole(const QString &strRole)
{
	m_pSessionController->setRole(strRole);
}

void KSessionViewModel::startSignalingServer(quint16 nPort)
{
	m_pSessionController->startSignalingServer(nPort);
}

void KSessionViewModel::connectSignaling(const QString &strHost, quint16 nPort)
{
	m_pSessionController->connectSignaling(strHost, nPort);
}

void KSessionViewModel::retryLastConnection()
{
	m_bEnterDesktopAfterReconnect = true;
	m_pSessionController->retryLastConnection();
}

void KSessionViewModel::disconnectSession()
{
	m_bEnterDesktopAfterReconnect = false;
	m_remoteScreenSize = QSize();
	m_inputFeedbackTracker.reset();
	m_pSessionController->disconnectSession();
	emit clearPreviewRequested();
}

void KSessionViewModel::enterRemoteDesktop()
{
	m_pSessionController->enterRemoteDesktop(m_streamConfig);
}

void KSessionViewModel::leaveRemoteDesktop()
{
	m_bEnterDesktopAfterReconnect = false;
	m_inputFeedbackTracker.reset();
	m_pSessionController->leaveRemoteDesktop();
	emit clearPreviewRequested();
}

void KSessionViewModel::startStreaming()
{
	m_pSessionController->startStreaming();
}

void KSessionViewModel::stopStreaming()
{
	m_pSessionController->stopStreaming();
}

void KSessionViewModel::sendRemoteMouseMove(int nX, int nY)
{
	KInputMessage message;
	message.type = MouseMoveInputMessageType;
	message.nX = nX;
	message.nY = nY;
	sendInputMessage(message, m_inputFeedbackTracker.shouldTraceMouseMove());
}

void KSessionViewModel::sendRemoteMouseButton(int nX, int nY, int nButton, bool bPressed)
{
	KRemoteMouseButton mouseButton = NoRemoteMouseButton;
	if (nButton == kRemoteMouseButtonLeft)
		mouseButton = LeftRemoteMouseButton;
	else if (nButton == kRemoteMouseButtonRight)
		mouseButton = RightRemoteMouseButton;
	else if (nButton == kRemoteMouseButtonMiddle)
		mouseButton = MiddleRemoteMouseButton;
	else if (nButton == kRemoteMouseButtonX1)
		mouseButton = X1RemoteMouseButton;
	else if (nButton == kRemoteMouseButtonX2)
		mouseButton = X2RemoteMouseButton;
	else
		return;

	KInputMessage message;
	message.type = MouseButtonInputMessageType;
	message.mouseButton = mouseButton;
	message.bPressed = bPressed;
	message.nX = nX;
	message.nY = nY;
	sendInputMessage(message, true);
}

void KSessionViewModel::sendRemoteMouseWheel(int nX, int nY, int nDelta)
{
	KInputMessage message;
	message.type = MouseWheelInputMessageType;
	message.nWheelDelta = nDelta;
	message.nX = nX;
	message.nY = nY;
	sendInputMessage(message, true);
}

void KSessionViewModel::sendRemoteKey(int nVirtualKey,
	int nScanCode,
	bool bPressed,
	bool bExtended,
	bool bAutoRepeat)
{
	if (nVirtualKey <= 0 || nVirtualKey > 0xFF)
		return;

	KInputMessage message;
	message.type = KeyInputMessageType;
	message.nVirtualKey = nVirtualKey;
	message.nScanCode = nScanCode;
	message.bPressed = bPressed;
	message.bExtended = bExtended;
	message.bAutoRepeat = bAutoRepeat;
	sendInputMessage(message, false);
}

void KSessionViewModel::sendRemoteText(const QString &strText)
{
	if (strText.isEmpty())
		return;
	if (strText.toUtf8().size() > KProtocolConstraints::kMaximumTextInputBytes)
	{
		emit errorOccurred(QStringLiteral("Committed text is too large to send"));
		return;
	}
	KInputMessage message;
	message.type = TextInputMessageType;
	message.strText = strText;
	sendInputMessage(message, false);
}

void KSessionViewModel::sendStreamConfig(const KStreamConfig &config)
{
	if (isSameStreamConfig(m_streamConfig, config))
		return;

	m_streamConfig = config;
	m_pSessionController->sendStreamConfig(config);
}

void KSessionViewModel::handleCaptureStatusChanged(const QString &strStatus)
{
	emit statusChanged(strStatus);
	if (strStatus == QStringLiteral("Stopped") || strStatus == QStringLiteral("Error"))
		emit clearPreviewRequested();
}

void KSessionViewModel::handleWebRtcStateChanged(const QString &strState)
{
	emit webRtcStateChanged(strState);
}

void KSessionViewModel::handleSessionStateChanged(KSessionState state)
{
	emit sessionStateChanged(state);
	const QString strPresentationState = state == IdleSessionState
		&& (m_lastSessionState == StoppingSessionState
			|| m_lastSessionState == ShutdownTimedOutSessionState)
		? QStringLiteral("Disconnected")
		: KSessionStateMachine::stateName(state);
	m_lastSessionState = state;
	emit webRtcStateChanged(strPresentationState);
	if (state == ConnectedSessionState && m_bEnterDesktopAfterReconnect)
	{
		m_bEnterDesktopAfterReconnect = false;
		enterRemoteDesktop();
		return;
	}
	if (state == ReconnectingSessionState
		|| state == ShutdownTimedOutSessionState)
	{
		m_inputFeedbackTracker.reset();
		emit suspendRemoteInputRequested();
	}
	else if (state == IdleSessionState || state == ListeningSessionState)
	{
		emit clearPreviewRequested();
	}

	if (state == IdleSessionState || state == ListeningSessionState)
	{
		m_bEnterDesktopAfterReconnect = false;
		m_remoteScreenSize = QSize();
		m_inputFeedbackTracker.reset();
		m_nPointerInputSequence = 0;
		m_nReliableInputSequence = 0;
	}
}

void KSessionViewModel::handleSessionError(const KSessionError &error)
{
	emit sessionErrorOccurred(error);
}

void KSessionViewModel::handleRemoteDeviceInfoChanged(const QString &strComputerName,
	const QString &strWallpaperMime,
	const QString &strWallpaperData,
	int nScreenWidth,
	int nScreenHeight)
{
	m_remoteScreenSize = QSize(qMax(0, nScreenWidth), qMax(0, nScreenHeight));
	emit remoteDeviceInfoChanged(strComputerName,
		strWallpaperMime,
		strWallpaperData,
		nScreenWidth,
		nScreenHeight);
}

void KSessionViewModel::initConnections()
{
	connect(m_pCaptureController, &KCaptureController::statusChanged,
		this, &KSessionViewModel::handleCaptureStatusChanged);
	connect(m_pCaptureController, &KCaptureController::captureError,
		this, &KSessionViewModel::errorOccurred);
	connect(m_pCaptureController, &KCaptureController::frameReady,
		this, &KSessionViewModel::frameReady);
	connect(m_pCaptureController, &KCaptureController::decodedFrameReady,
		this, &KSessionViewModel::renderFrameReady);
	connect(m_pSessionController, &KSessionController::signalingChanged,
		this, &KSessionViewModel::signalingChanged);
	connect(m_pSessionController, &KSessionController::webRtcStateChanged,
		this, &KSessionViewModel::handleWebRtcStateChanged);
	connect(m_pSessionController, &KSessionController::sessionStateChanged,
		this, &KSessionViewModel::handleSessionStateChanged);
	connect(m_pSessionController, &KSessionController::sessionChannelChanged,
		this, &KSessionViewModel::sessionChannelChanged);
	connect(m_pSessionController, &KSessionController::remoteDeviceInfoChanged,
		this, &KSessionViewModel::handleRemoteDeviceInfoChanged);
	connect(m_pSessionController, &KSessionController::sessionErrorOccurred,
		this, &KSessionViewModel::handleSessionError);
	connect(m_pSessionController, &KSessionController::remoteFrameReady,
		this, &KSessionViewModel::renderFrameReady);
	connect(m_pSessionController, &KSessionController::remoteFrameStatsReady,
		this, &KSessionViewModel::frameReady);
	connect(m_pSessionController, &KSessionController::networkStatsReady,
		this, &KSessionViewModel::networkStatsReady);
}

void KSessionViewModel::sendInputMessage(KInputMessage message, bool bTrace)
{
	if (m_pSessionController == nullptr)
		return;

	if (message.type == MouseMoveInputMessageType)
		message.nSequence = ++m_nPointerInputSequence * 2;
	else
		message.nSequence = ++m_nReliableInputSequence * 2 - 1;
	message.bTrace = bTrace;

	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("input_prepare"),
			QStringLiteral("seq=%1 type=%2 x=%3 y=%4")
				.arg(message.nSequence)
				.arg(KInputMessageCodec::typeName(message.type))
				.arg(message.nX)
				.arg(message.nY));
	}

	m_inputFeedbackTracker.recordInputSent(message);
	m_pSessionController->sendInputMessage(message);
}

void KSessionViewModel::handleInputFeedbackRendered(quint64 nSeq)
{
	m_inputFeedbackTracker.handleRendered(nSeq);
}
