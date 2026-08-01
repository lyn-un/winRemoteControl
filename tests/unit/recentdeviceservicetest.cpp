#include "adapters/settings/qsettingsrecentdevicestore.h"
#include "core/devices/recentdevicestore.h"
#include "devices/recentdeviceservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QTemporaryDir>
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

	class KFakeRecentDeviceStore final : public KRecentDeviceStore
	{
	public:
		QVector<KRecentDevice> loadDevices(QString *) override
		{
			return devices;
		}

		bool saveDevices(const QVector<KRecentDevice> &newDevices, QString *) override
		{
			devices = newDevices;
			++nSaveCount;
			return true;
		}

		QVector<KRecentDevice> devices;
		int nSaveCount = 0;
	};

	void completeConnection(KRecentDeviceService *pService,
		const QString &strHost,
		quint16 nPort,
		const QString &strName,
		bool bInfoFirst)
	{
		pService->connectEndpoint(strHost, nPort);
		if (bInfoFirst)
		{
			pService->setRemoteDeviceName(strName);
			pService->setSessionChannelOpen(true);
		}
		else
		{
			pService->setSessionChannelOpen(true);
			pService->setRemoteDeviceName(strName);
		}
		pService->setSessionChannelOpen(false);
	}

	void testSuccessfulConnectionAndDeduplication()
	{
		auto pStore = std::make_unique<KFakeRecentDeviceStore>();
		KFakeRecentDeviceStore *pFakeStore = pStore.get();
		KRecentDeviceService service(std::move(pStore));
		service.initialize();

		QVector<KRecentDevice> publishedDevices;
		QString strRequestedHost;
		quint16 nRequestedPort = 0;
		int nErrorCount = 0;
		QObject::connect(&service, &KRecentDeviceService::devicesChanged,
			[&publishedDevices](const QVector<KRecentDevice> &devices)
			{
				publishedDevices = devices;
			});
		QObject::connect(&service, &KRecentDeviceService::connectEndpointRequested,
			[&](const QString &strHost, quint16 nPort)
			{
				strRequestedHost = strHost;
				nRequestedPort = nPort;
			});
		QObject::connect(&service, &KRecentDeviceService::recentDeviceError,
			[&nErrorCount](const QString &) { ++nErrorCount; });

		service.connectEndpoint(QString(), 0);
		service.connectDevice(QStringLiteral("missing-device"));
		check(nErrorCount == 2,
			QStringLiteral("invalid endpoint and unknown recent ID are rejected"));

		service.connectEndpoint(QStringLiteral(" 192.168.1.20 "), 39000);
		check(strRequestedHost == QStringLiteral("192.168.1.20") && nRequestedPort == 39000,
			QStringLiteral("manual endpoint is normalized and forwarded"));
		service.setSessionChannelOpen(false);
		check(pFakeStore->devices.isEmpty(),
			QStringLiteral("failed connection is not persisted"));

		const int nSaveCountBeforeFirstSuccess = pFakeStore->nSaveCount;
		completeConnection(&service,
			QStringLiteral("192.168.1.20"), 39000, QStringLiteral("OFFICE-PC"), true);
		check(pFakeStore->devices.size() == 1
			&& pFakeStore->devices.first().strDeviceName == QStringLiteral("OFFICE-PC")
			&& pFakeStore->nSaveCount == nSaveCountBeforeFirstSuccess + 1,
			QStringLiteral("successful session stores remote device info"));

		const int nSaveCountBeforeUpdate = pFakeStore->nSaveCount;
		completeConnection(&service,
			QStringLiteral("192.168.1.20"), 39000, QStringLiteral("OFFICE-PC-NEW"), false);
		check(pFakeStore->devices.size() == 1
			&& pFakeStore->devices.first().strDeviceName == QStringLiteral("OFFICE-PC-NEW")
			&& pFakeStore->nSaveCount == nSaveCountBeforeUpdate + 1,
			QStringLiteral("same endpoint updates without duplication for either event order"));

		service.requestDevices();
		check(publishedDevices.size() == 1,
			QStringLiteral("request publishes loaded recent devices"));
		const QString strDeviceId = publishedDevices.first().strDeviceId;
		service.connectDevice(strDeviceId);
		check(strRequestedHost == QStringLiteral("192.168.1.20") && nRequestedPort == 39000,
			QStringLiteral("recent device ID resolves to native endpoint"));
		service.removeDevice(strDeviceId);
		check(pFakeStore->devices.isEmpty(),
			QStringLiteral("removing a recent device persists the new list"));
	}

	void testMaximumDeviceCount()
	{
		auto pStore = std::make_unique<KFakeRecentDeviceStore>();
		KFakeRecentDeviceStore *pFakeStore = pStore.get();
		KRecentDeviceService service(std::move(pStore));
		service.initialize();

		for (int nIndex = 0; nIndex < 10; ++nIndex)
		{
			completeConnection(&service,
				QStringLiteral("10.0.0.%1").arg(nIndex + 1),
				static_cast<quint16>(39000 + nIndex),
				QStringLiteral("PC-%1").arg(nIndex + 1),
				true);
			QThread::msleep(1);
		}
		check(pFakeStore->devices.size() == 8,
			QStringLiteral("recent device list is capped at eight entries"));
		check(pFakeStore->devices.first().strDeviceName == QStringLiteral("PC-10"),
			QStringLiteral("recent devices are sorted newest first"));
	}

	void testSettingsPersistence()
	{
		QTemporaryDir temporaryDir;
		check(temporaryDir.isValid(), QStringLiteral("temporary settings directory is available"));
		const QString strFilePath = temporaryDir.filePath(QStringLiteral("recent_devices.ini"));
		KQSettingsRecentDeviceStore store(strFilePath);

		KRecentDevice source;
		source.strDeviceId = QStringLiteral("device-id");
		source.strDeviceName = QStringLiteral("TEST-PC");
		source.strHost = QStringLiteral("172.16.0.8");
		source.nSignalingPort = 40100;
		source.nLastConnectedAtMs = 123456789;
		QString strError;
		check(store.saveDevices({ source }, &strError) && strError.isEmpty(),
			QStringLiteral("QSettings adapter saves devices"));
		const QVector<KRecentDevice> loaded = store.loadDevices(&strError);
		check(loaded.size() == 1
			&& loaded.first().strDeviceId == source.strDeviceId
			&& loaded.first().strDeviceName == source.strDeviceName
			&& loaded.first().strHost == source.strHost
			&& loaded.first().nSignalingPort == source.nSignalingPort
			&& loaded.first().nLastConnectedAtMs == source.nLastConnectedAtMs,
			QStringLiteral("QSettings adapter round-trips all recent device fields"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testSuccessfulConnectionAndDeduplication();
	testMaximumDeviceCount();
	testSettingsPersistence();
	if (g_nFailureCount == 0)
		qInfo() << "All recent device service tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
