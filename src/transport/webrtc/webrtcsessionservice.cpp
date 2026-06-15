#include "transport/webrtc/webrtcsessionservice.h"

#include "common/sessiontracelogger.h"
#include "input/inputinjector.h"
#include "transport/webrtc/webrtcsignaling.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSysInfo>
#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <Windows.h>

namespace
{
	constexpr int kWallpaperInitialMaxEdge = 640;
	constexpr int kWallpaperMinEdge = 240;
	constexpr int kWallpaperJpegQuality = 65;
	constexpr int kWallpaperMaxBase64Bytes = 96 * 1024;
	constexpr int kWallpaperPathBufferLength = MAX_PATH;

	constexpr char kType[] = "type";
	constexpr char kDeviceInfoRequest[] = "deviceInfoRequest";
	constexpr char kDeviceInfo[] = "deviceInfo";
	constexpr char kStartStreaming[] = "startStreaming";
	constexpr char kStopStreaming[] = "stopStreaming";
	constexpr char kStreamConfig[] = "streamConfig";
	constexpr char kComputerName[] = "computerName";
	constexpr char kWallpaperMime[] = "wallpaperMime";
	constexpr char kWallpaperData[] = "wallpaperData";
	constexpr char kScreenWidth[] = "screenWidth";
	constexpr char kScreenHeight[] = "screenHeight";
	constexpr char kFps[] = "fps";
	constexpr char kWidth[] = "width";
	constexpr char kHeight[] = "height";
	constexpr char kBitrateKbps[] = "bitrateKbps";

	static QString roleToString(const QString &strRole)
	{
		return strRole == QStringLiteral("controller")
			? QStringLiteral("controller")
			: QStringLiteral("controlled");
	}
}

KWebRtcSessionService::KWebRtcSessionService(QObject *pParent)
	: QObject(pParent)
	, m_pSignaling(new KWebRtcSignaling(this))
	, m_pInputInjector(new KInputInjector(this))
{
	connect(m_pSignaling, &KWebRtcSignaling::stateChanged,
		this, &KWebRtcSessionService::signalingChanged);
	connect(m_pSignaling, &KWebRtcSignaling::signalingError,
		this, &KWebRtcSessionService::sessionError);
	connect(m_pInputInjector, &KInputInjector::inputError,
		this, &KWebRtcSessionService::sessionError);
}

KWebRtcSessionService::~KWebRtcSessionService()
{
	disconnectSession();
}

void KWebRtcSessionService::setRole(const QString &strRole)
{
	if (strRole == QStringLiteral("controlled") || strRole == QStringLiteral("controller"))
		m_strRole = strRole;

	emit webRtcStateChanged(QStringLiteral("Role:%1").arg(m_strRole));
}

void KWebRtcSessionService::startSignalingServer(quint16 nPort)
{
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

	m_strRole = QStringLiteral("controlled");
}

void KWebRtcSessionService::connectSignaling(const QString &strHost, quint16 nPort)
{
	QString strError;
	if (!initializePeer(KWebRtcPeer::ControllerRole, &strError))
	{
		emit sessionError(strError);
		return;
	}

	if (!m_pSignaling->connectToHost(strHost, nPort, &strError))
	{
		emit sessionError(strError);
		return;
	}

	m_strRole = QStringLiteral("controller");
	m_pPeer->createOffer();
}

void KWebRtcSessionService::disconnectSession()
{
	m_bDeviceInfoRequested = false;
	m_nLastInjectedInputSeq = 0;
	m_nLastInjectedInputMs = -1;
	emit inputTraceUpdated(0, -1);
	emit stopCaptureRequested();
	if (m_pSignaling != nullptr)
		m_pSignaling->stop();
	if (m_pPeer != nullptr)
		m_pPeer->shutdown();
	emit webRtcStateChanged(QStringLiteral("Disconnected"));
}

void KWebRtcSessionService::enterRemoteDesktop()
{
	if (m_strRole != QStringLiteral("controller"))
		return;
	sendSessionMessage(createControlMessage(QString::fromLatin1(kStartStreaming)));
}

void KWebRtcSessionService::leaveRemoteDesktop()
{
	if (m_strRole != QStringLiteral("controller"))
		return;
	sendSessionMessage(createControlMessage(QString::fromLatin1(kStopStreaming)));
	emit stopCaptureRequested();
}

void KWebRtcSessionService::startStreaming()
{
	if (m_strRole != QStringLiteral("controlled"))
	{
		emit sessionError(QStringLiteral("只有被控端可以开始推流"));
		return;
	}

	emit startCaptureRequested();
	emit webRtcStateChanged(QStringLiteral("Streaming"));
}

void KWebRtcSessionService::stopStreaming()
{
	emit stopCaptureRequested();
	emit webRtcStateChanged(QStringLiteral("Stopped"));
}

void KWebRtcSessionService::pushVideoFrame(const KWebRtcVideoFrame &frame)
{
	if (m_pPeer != nullptr)
		m_pPeer->pushVideoFrame(frame);
}

void KWebRtcSessionService::sendInputMessage(const QString &strMessage)
{
	if (m_strRole != QStringLiteral("controller"))
		return;
	if (m_pPeer != nullptr)
		m_pPeer->sendInputMessage(strMessage);
}

void KWebRtcSessionService::sendSessionMessage(const QString &strMessage)
{
	if (m_pPeer != nullptr)
		m_pPeer->sendSessionMessage(strMessage);
}

void KWebRtcSessionService::sendStreamConfig(const KStreamConfig &config)
{
	if (m_strRole != QStringLiteral("controller"))
		return;
	sendSessionMessage(createStreamConfigMessage(config));
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
		this, &KWebRtcSessionService::remoteFrameReady);
	connect(m_pPeer, &KWebRtcPeer::remoteFrameStatsReady,
		this, &KWebRtcSessionService::remoteFrameStatsReady);
	connect(m_pPeer, &KWebRtcPeer::networkStatsReady,
		this, &KWebRtcSessionService::networkStatsReady);
	connect(m_pPeer, &KWebRtcPeer::inputMessageReceived,
		m_pInputInjector, &KInputInjector::handleInputMessage);
	connect(m_pInputInjector, &KInputInjector::inputInjected,
		this, &KWebRtcSessionService::handleInputInjected);
	connect(m_pPeer, &KWebRtcPeer::inputChannelChanged,
		this, &KWebRtcSessionService::inputChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::sessionMessageReceived,
		this, &KWebRtcSessionService::handleSessionMessage);
	connect(m_pPeer, &KWebRtcPeer::sessionChannelChanged,
		this, &KWebRtcSessionService::handleSessionChannelChanged);
	connect(m_pPeer, &KWebRtcPeer::sessionChannelChanged,
		this, &KWebRtcSessionService::sessionChannelChanged);
}

void KWebRtcSessionService::handleSessionChannelChanged(bool bOpen)
{
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("channel"),
		QStringLiteral("session"),
		-1,
		QStringLiteral("open=%1").arg(bOpen ? 1 : 0));
	if (!bOpen)
	{
		m_bDeviceInfoRequested = false;
		return;
	}
	if (m_strRole == QStringLiteral("controller"))
	{
		if (m_bDeviceInfoRequested)
		{
			KSessionTraceLogger::write(roleToString(m_strRole),
				QStringLiteral("skip"),
				QString::fromLatin1(kDeviceInfoRequest),
				-1,
				QStringLiteral("reason=already_requested"));
			return;
		}

		m_bDeviceInfoRequested = true;
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("handle_channel_open"),
			QString::fromLatin1(kDeviceInfoRequest));
		sendSessionMessage(createControlMessage(QString::fromLatin1(kDeviceInfoRequest)));
	}
}

void KWebRtcSessionService::handleSessionMessage(const QString &strMessage)
{
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return;

	const QJsonObject object = document.object();
	const QString strType = object.value(QString::fromLatin1(kType)).toString();
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("handle"),
		strType,
		strMessage.toUtf8().size());
	if (strType == QString::fromLatin1(kDeviceInfoRequest) && m_strRole == QStringLiteral("controlled"))
	{
		sendDeviceInfoMessage();
		return;
	}

	if (strType == QString::fromLatin1(kDeviceInfo) && m_strRole == QStringLiteral("controller"))
	{
		emit remoteDeviceInfoChanged(object.value(QString::fromLatin1(kComputerName)).toString(),
			object.value(QString::fromLatin1(kWallpaperMime)).toString(),
			object.value(QString::fromLatin1(kWallpaperData)).toString(),
			object.value(QString::fromLatin1(kScreenWidth)).toInt(),
			object.value(QString::fromLatin1(kScreenHeight)).toInt());
		return;
	}

	if (strType == QString::fromLatin1(kStartStreaming) && m_strRole == QStringLiteral("controlled"))
	{
		KSessionTraceLogger::write(roleToString(m_strRole),
			QStringLiteral("emit"),
			QStringLiteral("startCaptureRequested"));
		emit startCaptureRequested();
		emit webRtcStateChanged(QStringLiteral("Streaming"));
		return;
	}

	if (strType == QString::fromLatin1(kStopStreaming) && m_strRole == QStringLiteral("controlled"))
	{
		emit stopCaptureRequested();
		emit webRtcStateChanged(QStringLiteral("Stopped"));
		return;
	}

	if (strType == QString::fromLatin1(kStreamConfig) && m_strRole == QStringLiteral("controlled"))
	{
		const KStreamConfig config = streamConfigFromJson(object);
		if (m_pPeer != nullptr)
			m_pPeer->setStreamConfig(config);
		emit streamConfigChanged(config);
	}
}

void KWebRtcSessionService::handleInputInjected(quint64 nSeq, qint64 nInjectedMs)
{
	m_nLastInjectedInputSeq = nSeq;
	m_nLastInjectedInputMs = nInjectedMs;
	emit inputTraceUpdated(nSeq, nInjectedMs);
}

void KWebRtcSessionService::sendDeviceInfoMessage()
{
	QString strMimeType;
	const QString strWallpaperData = readWallpaperBase64(&strMimeType);
	const QString strMessage = createDeviceInfoMessage(strMimeType, strWallpaperData);
	KSessionTraceLogger::write(roleToString(m_strRole),
		QStringLiteral("prepare"),
		QString::fromLatin1(kDeviceInfo),
		strMessage.toUtf8().size(),
		QStringLiteral("wallpaper=%1 wallpaperBytes=%2 messageBytes=%3")
			.arg(strWallpaperData.isEmpty() ? 0 : 1)
			.arg(strWallpaperData.size())
			.arg(strMessage.toUtf8().size()));
	sendSessionMessage(strMessage);
}

QString KWebRtcSessionService::createDeviceInfoMessage(const QString &strWallpaperMime,
	const QString &strWallpaperData) const
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kType), QString::fromLatin1(kDeviceInfo));
	object.insert(QString::fromLatin1(kComputerName), QSysInfo::machineHostName());
	object.insert(QString::fromLatin1(kScreenWidth), ::GetSystemMetrics(SM_CXSCREEN));
	object.insert(QString::fromLatin1(kScreenHeight), ::GetSystemMetrics(SM_CYSCREEN));
	if (!strWallpaperData.isEmpty())
	{
		object.insert(QString::fromLatin1(kWallpaperMime), strWallpaperMime);
		object.insert(QString::fromLatin1(kWallpaperData), strWallpaperData);
	}

	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString KWebRtcSessionService::createControlMessage(const QString &strType) const
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kType), strType);
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString KWebRtcSessionService::createStreamConfigMessage(const KStreamConfig &config) const
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kType), QString::fromLatin1(kStreamConfig));
	object.insert(QString::fromLatin1(kFps), config.nFps);
	object.insert(QString::fromLatin1(kWidth), config.nWidth);
	object.insert(QString::fromLatin1(kHeight), config.nHeight);
	object.insert(QString::fromLatin1(kBitrateKbps), config.nBitrateKbps);
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

KStreamConfig KWebRtcSessionService::streamConfigFromJson(const QJsonObject &object)
{
	KStreamConfig config;
	config.nFps = object.value(QString::fromLatin1(kFps)).toInt(config.nFps);
	config.nWidth = object.value(QString::fromLatin1(kWidth)).toInt(config.nWidth);
	config.nHeight = object.value(QString::fromLatin1(kHeight)).toInt(config.nHeight);
	config.nBitrateKbps = object.value(QString::fromLatin1(kBitrateKbps)).toInt(config.nBitrateKbps);
	return config;
}

QString KWebRtcSessionService::readWallpaperBase64(QString *pMimeType)
{
	wchar_t szWallpaperPath[kWallpaperPathBufferLength] = {};
	if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER,
			kWallpaperPathBufferLength,
			szWallpaperPath,
			0))
	{
		return QString();
	}

	const QString strWallpaperPath = QString::fromWCharArray(szWallpaperPath);
	if (strWallpaperPath.isEmpty())
		return QString();

	QImageReader imageReader(strWallpaperPath);
	QImage image = imageReader.read();
	if (image.isNull())
		return QString();

	const int nMaxEdge = qMax(image.width(), image.height());
	if (nMaxEdge > kWallpaperInitialMaxEdge)
	{
		image = image.scaled(kWallpaperInitialMaxEdge,
			kWallpaperInitialMaxEdge,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	QByteArray base64Data;
	for (;;)
	{
		QByteArray imageData;
		QBuffer buffer(&imageData);
		if (!buffer.open(QIODevice::WriteOnly))
			return QString();
		if (!image.save(&buffer, "JPG", kWallpaperJpegQuality))
			return QString();

		base64Data = imageData.toBase64();
		if (base64Data.size() <= kWallpaperMaxBase64Bytes)
			break;

		const int nCurrentMaxEdge = qMax(image.width(), image.height());
		if (nCurrentMaxEdge <= kWallpaperMinEdge)
			return QString();

		const int nNextMaxEdge = qMax(kWallpaperMinEdge, nCurrentMaxEdge * 3 / 4);
		image = image.scaled(nNextMaxEdge,
			nNextMaxEdge,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	if (pMimeType != nullptr)
		*pMimeType = QStringLiteral("image/jpeg");
	return QString::fromLatin1(base64Data);
}
