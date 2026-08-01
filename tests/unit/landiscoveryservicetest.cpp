#include "adapters/discovery/udplandiscoverytransport.h"
#include "core/protocol/landiscoverymessage.h"
#include "discovery/landiscoveryservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

#include <memory>

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	class KFakeLanDiscoveryTransport final : public KLanDiscoveryTransport
	{
	public:
		bool start(quint16 nLocalPort, QString *) override
		{
			++nStartCount;
			nLastLocalPort = nLocalPort;
			bRunning = true;
			return true;
		}

		void sendBroadcast(const QByteArray &data, quint16 nPort) override
		{
			lastBroadcast = data;
			nLastBroadcastPort = nPort;
			++nBroadcastCount;
		}

		void sendUnicast(const QByteArray &data,
			const QString &strHost,
			quint16 nPort) override
		{
			lastUnicast = data;
			strLastUnicastHost = strHost;
			nLastUnicastPort = nPort;
			++nUnicastCount;
		}

		void stop() override
		{
			bRunning = false;
			++nStopCount;
		}

		void deliver(const QByteArray &data, const QString &strHost, quint16 nPort)
		{
			emit datagramReceived(data, strHost, nPort);
		}

		QByteArray lastBroadcast;
		QByteArray lastUnicast;
		QString strLastUnicastHost;
		quint16 nLastLocalPort = 0;
		quint16 nLastBroadcastPort = 0;
		quint16 nLastUnicastPort = 0;
		int nStartCount = 0;
		int nStopCount = 0;
		int nBroadcastCount = 0;
		int nUnicastCount = 0;
		bool bRunning = false;
	};

	void testControllerDiscoveryAndConnect()
	{
		auto pTransport = std::make_unique<KFakeLanDiscoveryTransport>();
		KFakeLanDiscoveryTransport *pFakeTransport = pTransport.get();
		KLanDiscoveryService service(std::move(pTransport),
			QStringLiteral("aaaaaaaa-1234-1234-1234-1234567890ab"),
			QStringLiteral("controller-host"));

		QVector<KDiscoveredDevice> devices;
		QString strConnectedHost;
		quint16 nConnectedPort = 0;
		int nDeviceChangeCount = 0;
		int nErrorCount = 0;
		QObject::connect(&service, &KLanDiscoveryService::devicesChanged,
			[&](const QVector<KDiscoveredDevice> &newDevices)
			{
				devices = newDevices;
				++nDeviceChangeCount;
			});
		QObject::connect(&service, &KLanDiscoveryService::connectEndpointRequested,
			[&](const QString &strHost, quint16 nPort)
			{
				strConnectedHost = strHost;
				nConnectedPort = nPort;
			});
		QObject::connect(&service, &KLanDiscoveryService::discoveryError,
			[&](const QString &) { ++nErrorCount; });

		service.setRole(ControllerSessionRole);
		check(pFakeTransport->nStartCount == 1 && pFakeTransport->nLastLocalPort == 0,
			QStringLiteral("controller discovery binds an ephemeral UDP port"));
		check(pFakeTransport->nBroadcastCount == 1
			&& pFakeTransport->nLastBroadcastPort == 39001,
			QStringLiteral("controller discovery probes immediately"));

		KLanDiscoveryMessage unrelatedAnnouncement;
		unrelatedAnnouncement.type = AnnounceLanDiscoveryMessageType;
		unrelatedAnnouncement.strRequestId = QStringLiteral("dddddddd-1234-1234-1234-1234567890ab");
		unrelatedAnnouncement.strInstanceId = QStringLiteral("eeeeeeee-1234-1234-1234-1234567890ab");
		unrelatedAnnouncement.strDeviceName = QStringLiteral("unrelated-host");
		unrelatedAnnouncement.nSignalingPort = 39000;
		pFakeTransport->deliver(KLanDiscoveryMessageCodec::encode(unrelatedAnnouncement),
			QStringLiteral("192.168.1.99"),
			39001);
		check(devices.isEmpty(),
			QStringLiteral("announcement for an unknown request ID is ignored"));

		KLanDiscoveryMessage probe;
		check(KLanDiscoveryMessageCodec::decode(
			pFakeTransport->lastBroadcast, &probe, nullptr),
			QStringLiteral("emitted discovery probe is valid"));
		KLanDiscoveryMessage announcement;
		announcement.type = AnnounceLanDiscoveryMessageType;
		announcement.strRequestId = probe.strRequestId;
		announcement.strInstanceId = QStringLiteral("bbbbbbbb-1234-1234-1234-1234567890ab");
		announcement.strDeviceName = QStringLiteral("controlled-host");
		announcement.nSignalingPort = 40123;
		pFakeTransport->deliver(KLanDiscoveryMessageCodec::encode(announcement),
			QStringLiteral("192.168.1.25"),
			39001);
		check(devices.size() == 1 && devices.first().strHost == QStringLiteral("192.168.1.25"),
			QStringLiteral("matching announcement uses the datagram source address"));

		pFakeTransport->deliver(KLanDiscoveryMessageCodec::encode(announcement),
			QStringLiteral("192.168.1.25"),
			39001);
		check(devices.size() == 1 && nDeviceChangeCount == 1,
			QStringLiteral("duplicate announcement refreshes without adding a device"));
		service.connectDevice(announcement.strInstanceId);
		check(strConnectedHost == QStringLiteral("192.168.1.25") && nConnectedPort == 40123,
			QStringLiteral("one-click connect resolves the native discovery record"));
		service.connectDevice(QStringLiteral("missing"));
		check(nErrorCount == 1, QStringLiteral("unknown device cannot start a connection"));
	}

	void testControlledResponderAvailability()
	{
		auto pTransport = std::make_unique<KFakeLanDiscoveryTransport>();
		KFakeLanDiscoveryTransport *pFakeTransport = pTransport.get();
		KLanDiscoveryService service(std::move(pTransport),
			QStringLiteral("aaaaaaaa-1234-1234-1234-1234567890ab"),
			QStringLiteral("controlled-host"));

		service.setRole(ControlledSessionRole);
		check(!pFakeTransport->bRunning,
			QStringLiteral("controlled discovery stays stopped before TCP listening"));
		service.setListeningAvailability(true, 39000);
		check(pFakeTransport->bRunning && pFakeTransport->nLastLocalPort == 39001,
			QStringLiteral("controlled discovery binds the fixed responder port"));

		KLanDiscoveryMessage probe;
		probe.type = ProbeLanDiscoveryMessageType;
		probe.strRequestId = QStringLiteral("cccccccc-1234-1234-1234-1234567890ab");
		pFakeTransport->deliver(KLanDiscoveryMessageCodec::encode(probe),
			QStringLiteral("10.0.0.12"),
			51234);
		KLanDiscoveryMessage announcement;
		check(pFakeTransport->nUnicastCount == 1
			&& pFakeTransport->strLastUnicastHost == QStringLiteral("10.0.0.12")
			&& pFakeTransport->nLastUnicastPort == 51234
			&& KLanDiscoveryMessageCodec::decode(
				pFakeTransport->lastUnicast, &announcement, nullptr)
			&& announcement.nSignalingPort == 39000,
			QStringLiteral("controlled responder unicasts its active signaling endpoint"));

		service.setListeningAvailability(false, 0);
		const int nUnicastCount = pFakeTransport->nUnicastCount;
		pFakeTransport->deliver(KLanDiscoveryMessageCodec::encode(probe),
			QStringLiteral("10.0.0.12"),
			51234);
		check(!pFakeTransport->bRunning && pFakeTransport->nUnicastCount == nUnicastCount,
			QStringLiteral("active session pauses discovery replies"));
	}

	void testUdpLoopbackDiscovery()
	{
		KLanDiscoveryService responder(std::make_unique<KUdpLanDiscoveryTransport>(),
			QStringLiteral("ffffffff-1234-1234-1234-1234567890ab"),
			QStringLiteral("loopback-controlled"));
		responder.setRole(ControlledSessionRole);
		responder.setListeningAvailability(true, 39000);

		KLanDiscoveryService scanner(std::make_unique<KUdpLanDiscoveryTransport>(),
			QStringLiteral("99999999-1234-1234-1234-1234567890ab"),
			QStringLiteral("loopback-controller"));
		QVector<KDiscoveredDevice> devices;
		int nDiscoveryErrorCount = 0;
		QObject::connect(&scanner, &KLanDiscoveryService::devicesChanged,
			[&devices](const QVector<KDiscoveredDevice> &newDevices)
			{
				devices = newDevices;
			});
		QObject::connect(&scanner, &KLanDiscoveryService::discoveryError,
			[&nDiscoveryErrorCount](const QString &) { ++nDiscoveryErrorCount; });
		scanner.setRole(ControllerSessionRole);

		QElapsedTimer timer;
		timer.start();
		while (devices.isEmpty() && timer.elapsed() < 1000)
		{
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
		check(devices.size() == 1
			&& !devices.first().strHost.isEmpty()
			&& devices.first().nSignalingPort == 39000,
			QStringLiteral("real UDP adapters discover a loopback responder"));
		QElapsedTimer settleTimer;
		settleTimer.start();
		while (settleTimer.elapsed() < 100)
		{
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
		check(nDiscoveryErrorCount == 0,
			QStringLiteral("Windows UDP probes do not surface ICMP reset errors"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testControllerDiscoveryAndConnect();
	testControlledResponderAvailability();
	testUdpLoopbackDiscovery();
	if (g_nFailureCount == 0)
		qInfo() << "All LAN discovery service tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
