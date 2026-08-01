#include "discovery/landiscoveryservice.h"

#include "common/sessiontracelogger.h"
#include "core/protocol/landiscoverymessage.h"

#include <QtCore/QUuid>

#include <algorithm>

KLanDiscoveryService::KLanDiscoveryService(
	std::unique_ptr<KLanDiscoveryTransport> pTransport,
	const QString &strInstanceId,
	const QString &strDeviceName,
	QObject *pParent)
	: KDeviceDiscoveryController(pParent)
	, m_pTransport(std::move(pTransport))
	, m_strInstanceId(strInstanceId)
	, m_strDeviceName(strDeviceName.left(KLanDiscoveryMessageCodec::kMaximumDeviceNameLength))
{
	m_clock.start();
	m_probeTimer.setInterval(kProbeIntervalMs);
	m_expiryTimer.setInterval(1000);
	connect(&m_probeTimer, &QTimer::timeout, this, &KLanDiscoveryService::sendProbe);
	connect(&m_expiryTimer, &QTimer::timeout,
		this, &KLanDiscoveryService::removeExpiredDevices);
	connect(m_pTransport.get(), &KLanDiscoveryTransport::datagramReceived,
		this, &KLanDiscoveryService::handleDatagram);
	connect(m_pTransport.get(), &KLanDiscoveryTransport::transportError,
		this, &KLanDiscoveryService::handleTransportError);
}

KLanDiscoveryService::~KLanDiscoveryService()
{
	stop();
}

void KLanDiscoveryService::setRole(KSessionRole role)
{
	if (m_bRunning && m_role == role)
		return;

	stop();
	m_role = role;
	if (m_role == ControllerSessionRole)
		startControllerDiscovery();
	else if (m_bListeningAvailable)
		startControlledResponder();
}

void KLanDiscoveryService::setListeningAvailability(bool bAvailable, quint16 nPort)
{
	m_bListeningAvailable = bAvailable;
	m_nSignalingPort = bAvailable ? nPort : 0;
	if (m_role != ControlledSessionRole)
		return;

	if (!bAvailable)
	{
		if (m_bRunning)
			stop();
		return;
	}
	if (!m_bRunning)
		startControlledResponder();
}

void KLanDiscoveryService::refresh()
{
	if (m_role != ControllerSessionRole)
		return;
	if (!m_bRunning)
		startControllerDiscovery();
	else
		sendProbe();
}

void KLanDiscoveryService::connectDevice(const QString &strDeviceId)
{
	const auto it = m_devices.constFind(strDeviceId);
	if (it == m_devices.cend()
		|| m_clock.elapsed() - it->nLastSeenMs >= kDeviceExpiryMs)
	{
		emit discoveryError(QStringLiteral("设备已离线，请刷新后重试"));
		writeTrace(QStringLiteral("discovery_error"), QStringLiteral("reason=device_not_found"));
		return;
	}

	writeTrace(QStringLiteral("discovery_connect"),
		QStringLiteral("deviceId=%1 host=%2 port=%3")
			.arg(it->strDeviceId, it->strHost)
			.arg(it->nSignalingPort));
	emit connectEndpointRequested(it->strHost, it->nSignalingPort);
}

void KLanDiscoveryService::stop()
{
	const bool bWasRunning = m_bRunning;
	m_probeTimer.stop();
	m_expiryTimer.stop();
	m_recentRequestIds.clear();
	if (m_pTransport != nullptr)
		m_pTransport->stop();
	m_bRunning = false;
	clearDevices();
	if (bWasRunning)
		writeTrace(QStringLiteral("discovery_stop"));
}

void KLanDiscoveryService::handleDatagram(const QByteArray &data,
	const QString &strSenderHost,
	quint16 nSenderPort)
{
	KLanDiscoveryMessage message;
	QString strError;
	if (!KLanDiscoveryMessageCodec::decode(data, &message, &strError))
		return;

	if (m_role == ControllerSessionRole
		&& message.type == AnnounceLanDiscoveryMessageType
		&& m_recentRequestIds.contains(message.strRequestId))
	{
		handleAnnouncement(message, strSenderHost);
	}
	else if (m_role == ControlledSessionRole
		&& m_bListeningAvailable
		&& message.type == ProbeLanDiscoveryMessageType)
	{
		handleProbe(message, strSenderHost, nSenderPort);
	}
}

void KLanDiscoveryService::removeExpiredDevices()
{
	bool bChanged = false;
	const qint64 nNowMs = m_clock.elapsed();
	for (auto it = m_devices.begin(); it != m_devices.end();)
	{
		if (nNowMs - it->nLastSeenMs < kDeviceExpiryMs)
		{
			++it;
			continue;
		}
		writeTrace(QStringLiteral("discovery_device_expired"),
			QStringLiteral("deviceId=%1").arg(it->strDeviceId));
		it = m_devices.erase(it);
		bChanged = true;
	}
	if (bChanged)
		publishDevices();
}

void KLanDiscoveryService::handleTransportError(const QString &strError)
{
	writeTrace(QStringLiteral("discovery_error"),
		QStringLiteral("error=%1").arg(strError));
	emit discoveryError(strError);
}

bool KLanDiscoveryService::startTransport(quint16 nLocalPort)
{
	QString strError;
	if (!m_pTransport->start(nLocalPort, &strError))
	{
		handleTransportError(strError);
		return false;
	}
	m_bRunning = true;
	return true;
}

void KLanDiscoveryService::startControllerDiscovery()
{
	if (!startTransport(0))
		return;
	m_probeTimer.start();
	m_expiryTimer.start();
	writeTrace(QStringLiteral("discovery_start"), QStringLiteral("mode=scanner"));
	sendProbe();
}

void KLanDiscoveryService::startControlledResponder()
{
	if (m_nSignalingPort == 0 || !startTransport(kDiscoveryPort))
		return;
	writeTrace(QStringLiteral("discovery_start"),
		QStringLiteral("mode=responder udpPort=%1 signalingPort=%2")
			.arg(kDiscoveryPort)
			.arg(m_nSignalingPort));
}

void KLanDiscoveryService::sendProbe()
{
	if (!m_bRunning || m_role != ControllerSessionRole)
		return;
	KLanDiscoveryMessage message;
	message.type = ProbeLanDiscoveryMessageType;
	message.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_recentRequestIds.append(message.strRequestId);
	while (m_recentRequestIds.size() > 3)
		m_recentRequestIds.removeFirst();
	m_pTransport->sendBroadcast(KLanDiscoveryMessageCodec::encode(message), kDiscoveryPort);
}

void KLanDiscoveryService::handleProbe(const KLanDiscoveryMessage &message,
	const QString &strSenderHost,
	quint16 nSenderPort)
{
	KLanDiscoveryMessage announcement;
	announcement.type = AnnounceLanDiscoveryMessageType;
	announcement.strRequestId = message.strRequestId;
	announcement.strInstanceId = m_strInstanceId;
	announcement.strDeviceName = m_strDeviceName.isEmpty()
		? QStringLiteral("Windows Device")
		: m_strDeviceName;
	announcement.nSignalingPort = m_nSignalingPort;
	m_pTransport->sendUnicast(KLanDiscoveryMessageCodec::encode(announcement),
		strSenderHost,
		nSenderPort);
}

void KLanDiscoveryService::handleAnnouncement(const KLanDiscoveryMessage &message,
	const QString &strSenderHost)
{
	const bool bAdded = !m_devices.contains(message.strInstanceId);
	KDiscoveredDevice &device = m_devices[message.strInstanceId];
	const bool bEndpointChanged = device.strHost != strSenderHost
		|| device.nSignalingPort != message.nSignalingPort
		|| device.strDeviceName != message.strDeviceName;
	device.strDeviceId = message.strInstanceId;
	device.strDeviceName = message.strDeviceName;
	device.strHost = strSenderHost;
	device.nSignalingPort = message.nSignalingPort;
	device.nLastSeenMs = m_clock.elapsed();
	if (bAdded)
	{
		writeTrace(QStringLiteral("discovery_device_added"),
			QStringLiteral("deviceId=%1 host=%2 port=%3")
				.arg(device.strDeviceId, device.strHost)
				.arg(device.nSignalingPort));
	}
	if (bAdded || bEndpointChanged)
		publishDevices();
}

void KLanDiscoveryService::clearDevices()
{
	if (m_devices.isEmpty())
		return;
	m_devices.clear();
	publishDevices();
}

void KLanDiscoveryService::publishDevices()
{
	QVector<KDiscoveredDevice> devices = m_devices.values().toVector();
	std::sort(devices.begin(), devices.end(), [](const auto &left, const auto &right) {
		const int nNameCompare = QString::localeAwareCompare(left.strDeviceName,
			right.strDeviceName);
		return nNameCompare != 0 ? nNameCompare < 0 : left.strHost < right.strHost;
	});
	emit devicesChanged(devices);
}

void KLanDiscoveryService::writeTrace(const QString &strStage, const QString &strExtra) const
{
	KSessionTraceLogger::write(KSessionStateMachine::roleName(m_role),
		strStage,
		QStringLiteral("lan_discovery"),
		-1,
		strExtra);
}
