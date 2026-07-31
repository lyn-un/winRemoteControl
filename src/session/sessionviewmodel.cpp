#include "session/sessionviewmodel.h"

#include "capture/captureservice.h"
#include "common/latencytracelogger.h"
#include "transport/webrtc/webrtcsessionservice.h"

#include <algorithm>

namespace
{
	constexpr int kRemoteMouseButtonLeft = 1;
	constexpr int kRemoteMouseButtonRight = 2;
	constexpr qint64 kInputMoveTraceIntervalMs = 500;
	constexpr qsizetype kMaxPendingInputTraceCount = 2048;
	constexpr qsizetype kInputRoundTripWindowSize = 120;
	constexpr quint64 kInputRoundTripStatsInterval = 30;
	bool isSameStreamConfig(const KStreamConfig &lhs, const KStreamConfig &rhs)
	{
		return lhs.nFps == rhs.nFps
			&& lhs.nWidth == rhs.nWidth
			&& lhs.nHeight == rhs.nHeight
			&& lhs.nBitrateKbps == rhs.nBitrateKbps;
	}
}

KSessionViewModel::KSessionViewModel(QObject *pParent)
	: QObject(pParent)
	, m_pCaptureService(new KCaptureService(this))
	, m_pWebRtcSessionService(new KWebRtcSessionService(this))
{
	initConnections();
}

KSessionViewModel::~KSessionViewModel()
{
	disconnectSession();
	m_pCaptureService->stopCapture();
}

QSize KSessionViewModel::remoteScreenSize() const
{
	return m_remoteScreenSize;
}

void KSessionViewModel::startLocalPreview()
{
	m_pCaptureService->startCapture();
}

void KSessionViewModel::stopCapture()
{
	m_pCaptureService->stopCapture();
}

void KSessionViewModel::setRole(const QString &strRole)
{
	m_pWebRtcSessionService->setRole(strRole);
}

void KSessionViewModel::startSignalingServer(quint16 nPort)
{
	m_pWebRtcSessionService->startSignalingServer(nPort);
}

void KSessionViewModel::connectSignaling(const QString &strHost, quint16 nPort)
{
	m_pWebRtcSessionService->connectSignaling(strHost, nPort);
}

void KSessionViewModel::disconnectSession()
{
	m_remoteScreenSize = QSize();
	resetInputRoundTripTrace();
	m_pWebRtcSessionService->disconnectSession();
	emit clearPreviewRequested();
}

void KSessionViewModel::enterRemoteDesktop()
{
	m_pWebRtcSessionService->enterRemoteDesktop(m_streamConfig);
}

void KSessionViewModel::leaveRemoteDesktop()
{
	resetInputRoundTripTrace();
	m_pWebRtcSessionService->leaveRemoteDesktop();
	emit clearPreviewRequested();
}

void KSessionViewModel::startStreaming()
{
	m_pWebRtcSessionService->startStreaming();
}

void KSessionViewModel::stopStreaming()
{
	m_pWebRtcSessionService->stopStreaming();
}

void KSessionViewModel::sendRemoteMouseMove(int nX, int nY)
{
	KInputMessage message;
	message.type = MouseMoveInputMessageType;
	message.nX = nX;
	message.nY = nY;
	sendInputMessage(message, shouldTraceMouseMove());
}

void KSessionViewModel::sendRemoteMouseButton(int nX, int nY, int nButton, bool bPressed)
{
	KRemoteMouseButton mouseButton = NoRemoteMouseButton;
	if (nButton == kRemoteMouseButtonLeft)
		mouseButton = LeftRemoteMouseButton;
	else if (nButton == kRemoteMouseButtonRight)
		mouseButton = RightRemoteMouseButton;
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

void KSessionViewModel::sendRemoteKey(int nVirtualKey, bool bPressed, bool bExtended)
{
	if (nVirtualKey <= 0 || nVirtualKey > 0xFF)
		return;

	KInputMessage message;
	message.type = KeyInputMessageType;
	message.nVirtualKey = nVirtualKey;
	message.bPressed = bPressed;
	message.bExtended = bExtended;
	sendInputMessage(message, false);
}

void KSessionViewModel::sendStreamConfig(const KStreamConfig &config)
{
	if (isSameStreamConfig(m_streamConfig, config))
		return;

	m_streamConfig = config;
	m_pWebRtcSessionService->sendStreamConfig(config);
}

void KSessionViewModel::handleCaptureStatusChanged(const QString &strStatus)
{
	emit statusChanged(strStatus);
	if (strStatus == QStringLiteral("Stopped") || strStatus == QStringLiteral("Error"))
		emit clearPreviewRequested();
	if (strStatus == QStringLiteral("Error"))
		m_pWebRtcSessionService->handleCaptureFailure();
}

void KSessionViewModel::handleWebRtcStateChanged(const QString &strState)
{
	emit webRtcStateChanged(strState);
	if (strState == QStringLiteral("Interrupted")
		|| strState == QStringLiteral("Disconnected")
		|| strState == QStringLiteral("Listening")
		|| strState == QStringLiteral("Failed")
		|| strState == QStringLiteral("Stopped"))
	{
		emit clearPreviewRequested();
	}

	if (strState == QStringLiteral("Disconnected")
		|| strState == QStringLiteral("Listening")
		|| strState == QStringLiteral("Failed"))
	{
		m_remoteScreenSize = QSize();
		resetInputRoundTripTrace();
	}
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
	connect(m_pCaptureService, &KCaptureService::statusChanged,
		this, &KSessionViewModel::handleCaptureStatusChanged);
	connect(m_pCaptureService, &KCaptureService::captureError,
		this, &KSessionViewModel::errorOccurred);
	connect(m_pCaptureService, &KCaptureService::frameReady,
		this, &KSessionViewModel::frameReady);
	connect(m_pCaptureService, &KCaptureService::decodedFrameReady,
		this, &KSessionViewModel::renderFrameReady);
	connect(m_pCaptureService, &KCaptureService::webRtcFrameReady,
		m_pWebRtcSessionService, &KWebRtcSessionService::pushVideoFrame);

	connect(m_pWebRtcSessionService, &KWebRtcSessionService::startCaptureRequested,
		m_pCaptureService, &KCaptureService::startWebRtcCapture);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::stopCaptureRequested,
		m_pCaptureService, &KCaptureService::stopCapture);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::streamConfigChanged,
		m_pCaptureService, &KCaptureService::setStreamConfig);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::inputTraceUpdated,
		m_pCaptureService, &KCaptureService::setInputTraceState);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::inputFeedbackFrameRequested,
		m_pCaptureService, &KCaptureService::requestImmediateFrame);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::signalingChanged,
		this, &KSessionViewModel::signalingChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::webRtcStateChanged,
		this, &KSessionViewModel::handleWebRtcStateChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::sessionChannelChanged,
		this, &KSessionViewModel::sessionChannelChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::remoteDeviceInfoChanged,
		this, &KSessionViewModel::handleRemoteDeviceInfoChanged);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::sessionError,
		this, &KSessionViewModel::errorOccurred);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::remoteFrameReady,
		this, &KSessionViewModel::renderFrameReady);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::remoteFrameStatsReady,
		this, &KSessionViewModel::frameReady);
	connect(m_pWebRtcSessionService, &KWebRtcSessionService::networkStatsReady,
		this, &KSessionViewModel::networkStatsReady);
}

void KSessionViewModel::sendInputMessage(KInputMessage message, bool bTrace)
{
	if (m_pWebRtcSessionService == nullptr)
		return;

	message.nSequence = ++m_nInputSequence;
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

	recordInputSent(message);
	m_pWebRtcSessionService->sendInputMessage(message);
}

void KSessionViewModel::handleInputFeedbackRendered(quint64 nSeq)
{
	if (!KLatencyTraceLogger::isEnabled() || !m_inputRoundTripTimer.isValid() || nSeq == 0)
		return;

	const auto sentTraceIt = m_inputSentTraces.constFind(nSeq);
	const bool bFoundSentTrace = sentTraceIt != m_inputSentTraces.constEnd();
	const KPendingInputTrace sentTrace =
		bFoundSentTrace ? sentTraceIt.value() : KPendingInputTrace();
	while (!m_inputSentTraces.isEmpty() && m_inputSentTraces.firstKey() <= nSeq)
		m_inputSentTraces.erase(m_inputSentTraces.begin());

	if (!bFoundSentTrace)
		return;

	const qint64 nRoundTripMs = m_inputRoundTripTimer.elapsed() - sentTrace.nSentMs;
	if (nRoundTripMs < 0)
		return;

	const bool bKeyInput = sentTrace.strType == KInputMessageCodec::typeName(KeyInputMessageType);
	const bool bIncludeInStats = !bKeyInput || sentTrace.bKeyPressed;
	const QString strPressed = bKeyInput
		? QStringLiteral(" pressed=%1").arg(sentTrace.bKeyPressed ? 1 : 0)
		: QString();
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("input_roundtrip"),
		QStringLiteral("seq=%1 type=%2%3 roundTripMs=%4 includedInStats=%5")
			.arg(nSeq)
			.arg(sentTrace.strType)
			.arg(strPressed)
			.arg(nRoundTripMs)
			.arg(bIncludeInStats ? 1 : 0));

	if (!bIncludeInStats)
		return;
	if (m_inputRoundTripSamples.size() >= kInputRoundTripWindowSize)
		m_inputRoundTripSamples.remove(0);
	m_inputRoundTripSamples.append(nRoundTripMs);
	++m_nInputRoundTripSampleCount;
	if (m_nInputRoundTripSampleCount % kInputRoundTripStatsInterval == 0)
		logInputRoundTripStats();
}

void KSessionViewModel::resetInputRoundTripTrace()
{
	m_inputSentTraces.clear();
	m_inputRoundTripSamples.clear();
	m_nInputRoundTripSampleCount = 0;
	m_inputRoundTripTimer.invalidate();
}

void KSessionViewModel::recordInputSent(const KInputMessage &message)
{
	if (!KLatencyTraceLogger::isEnabled())
		return;

	if (!m_inputRoundTripTimer.isValid())
		m_inputRoundTripTimer.start();
	while (m_inputSentTraces.size() >= kMaxPendingInputTraceCount)
		m_inputSentTraces.erase(m_inputSentTraces.begin());

	KPendingInputTrace trace;
	trace.nSentMs = m_inputRoundTripTimer.elapsed();
	trace.strType = KInputMessageCodec::typeName(message.type);
	trace.bKeyPressed = message.bPressed;
	m_inputSentTraces.insert(message.nSequence, trace);
}

void KSessionViewModel::logInputRoundTripStats()
{
	if (m_inputRoundTripSamples.isEmpty())
		return;

	QVector<qint64> sortedSamples = m_inputRoundTripSamples;
	std::sort(sortedSamples.begin(), sortedSamples.end());
	qint64 nTotalMs = 0;
	for (const qint64 nSampleMs : sortedSamples)
		nTotalMs += nSampleMs;

	const qsizetype nP50Index = (sortedSamples.size() - 1) * 50 / 100;
	const qsizetype nP95Index = (sortedSamples.size() * 95 + 99) / 100 - 1;
	const qint64 nAverageMs = (nTotalMs + sortedSamples.size() / 2) / sortedSamples.size();
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("input_roundtrip_stats"),
		QStringLiteral("scope=exclude_key_release samples=%1 totalSamples=%2 avgMs=%3 p50Ms=%4 p95Ms=%5 maxMs=%6")
			.arg(sortedSamples.size())
			.arg(m_nInputRoundTripSampleCount)
			.arg(nAverageMs)
			.arg(sortedSamples.at(nP50Index))
			.arg(sortedSamples.at(nP95Index))
			.arg(sortedSamples.constLast()));
}

bool KSessionViewModel::shouldTraceMouseMove()
{
	if (!KLatencyTraceLogger::isEnabled())
		return false;

	if (!m_inputMoveTraceTimer.isValid())
	{
		m_inputMoveTraceTimer.start();
		return true;
	}

	if (m_inputMoveTraceTimer.elapsed() < kInputMoveTraceIntervalMs)
		return false;

	m_inputMoveTraceTimer.restart();
	return true;
}
