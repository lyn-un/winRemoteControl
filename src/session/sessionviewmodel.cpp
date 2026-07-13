#include "session/sessionviewmodel.h"

#include "capture/captureservice.h"
#include "common/latencytracelogger.h"
#include "transport/webrtc/webrtcsessionservice.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace
{
	constexpr int kRemoteMouseButtonLeft = 1;
	constexpr int kRemoteMouseButtonRight = 2;
	constexpr qint64 kInputMoveTraceIntervalMs = 500;
	constexpr qsizetype kMaxPendingInputTraceCount = 2048;
	constexpr qsizetype kInputRoundTripWindowSize = 120;
	constexpr quint64 kInputRoundTripStatsInterval = 30;
	constexpr char kType[] = "type";
	constexpr char kX[] = "x";
	constexpr char kY[] = "y";
	constexpr char kSeq[] = "seq";
	constexpr char kTrace[] = "trace";
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
	m_pWebRtcSessionService->enterRemoteDesktop();
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
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("mouseMove"));
	object.insert(QStringLiteral("x"), nX);
	object.insert(QStringLiteral("y"), nY);
	sendInputJsonMessage(object, shouldTraceMouseMove());
}

void KSessionViewModel::sendRemoteMouseButton(int nX, int nY, int nButton, bool bPressed)
{
	QString strButton;
	if (nButton == kRemoteMouseButtonLeft)
		strButton = QStringLiteral("left");
	else if (nButton == kRemoteMouseButtonRight)
		strButton = QStringLiteral("right");
	else
		return;

	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("mouseButton"));
	object.insert(QStringLiteral("button"), strButton);
	object.insert(QStringLiteral("pressed"), bPressed);
	object.insert(QStringLiteral("x"), nX);
	object.insert(QStringLiteral("y"), nY);
	sendInputJsonMessage(object, true);
}

void KSessionViewModel::sendRemoteMouseWheel(int nX, int nY, int nDelta)
{
	QJsonObject object;
	object.insert(QStringLiteral("type"), QStringLiteral("mouseWheel"));
	object.insert(QStringLiteral("delta"), nDelta);
	object.insert(QStringLiteral("x"), nX);
	object.insert(QStringLiteral("y"), nY);
	sendInputJsonMessage(object, true);
}

void KSessionViewModel::sendStreamConfig(const KStreamConfig &config)
{
	m_pWebRtcSessionService->sendStreamConfig(config);
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
	if (strState == QStringLiteral("Disconnected") || strState == QStringLiteral("Stopped"))
		emit clearPreviewRequested();
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

void KSessionViewModel::sendInputJsonMessage(QJsonObject object, bool bTrace)
{
	if (m_pWebRtcSessionService == nullptr)
		return;

	const quint64 nSeq = ++m_nInputSequence;
	object.insert(QString::fromLatin1(kSeq), QString::number(nSeq));
	if (bTrace)
		object.insert(QString::fromLatin1(kTrace), true);

	if (bTrace)
	{
		KLatencyTraceLogger::write(QStringLiteral("controller"),
			QStringLiteral("input_prepare"),
			QStringLiteral("seq=%1 type=%2 x=%3 y=%4")
				.arg(nSeq)
				.arg(object.value(QString::fromLatin1(kType)).toString())
				.arg(object.value(QString::fromLatin1(kX)).toInt())
				.arg(object.value(QString::fromLatin1(kY)).toInt()));
	}

	recordInputSent(nSeq);
	m_pWebRtcSessionService->sendInputMessage(
		QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void KSessionViewModel::handleInputFeedbackRendered(quint64 nSeq)
{
	if (!KLatencyTraceLogger::isEnabled() || !m_inputRoundTripTimer.isValid() || nSeq == 0)
		return;

	const auto sentTimeIt = m_inputSentTimesMs.constFind(nSeq);
	const qint64 nSentMs = sentTimeIt != m_inputSentTimesMs.constEnd() ? sentTimeIt.value() : -1;
	while (!m_inputSentTimesMs.isEmpty() && m_inputSentTimesMs.firstKey() <= nSeq)
		m_inputSentTimesMs.erase(m_inputSentTimesMs.begin());

	if (nSentMs < 0)
		return;

	const qint64 nRoundTripMs = m_inputRoundTripTimer.elapsed() - nSentMs;
	if (nRoundTripMs < 0)
		return;

	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("input_roundtrip"),
		QStringLiteral("seq=%1 roundTripMs=%2").arg(nSeq).arg(nRoundTripMs));

	if (m_inputRoundTripSamples.size() >= kInputRoundTripWindowSize)
		m_inputRoundTripSamples.remove(0);
	m_inputRoundTripSamples.append(nRoundTripMs);
	++m_nInputRoundTripSampleCount;
	if (m_nInputRoundTripSampleCount % kInputRoundTripStatsInterval == 0)
		logInputRoundTripStats();
}

void KSessionViewModel::resetInputRoundTripTrace()
{
	m_inputSentTimesMs.clear();
	m_inputRoundTripSamples.clear();
	m_nInputRoundTripSampleCount = 0;
	m_inputRoundTripTimer.invalidate();
}

void KSessionViewModel::recordInputSent(quint64 nSeq)
{
	if (!KLatencyTraceLogger::isEnabled())
		return;

	if (!m_inputRoundTripTimer.isValid())
		m_inputRoundTripTimer.start();
	while (m_inputSentTimesMs.size() >= kMaxPendingInputTraceCount)
		m_inputSentTimesMs.erase(m_inputSentTimesMs.begin());
	m_inputSentTimesMs.insert(nSeq, m_inputRoundTripTimer.elapsed());
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
		QStringLiteral("samples=%1 totalSamples=%2 avgMs=%3 p50Ms=%4 p95Ms=%5 maxMs=%6")
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
