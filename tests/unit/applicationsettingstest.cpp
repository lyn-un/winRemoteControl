#include "adapters/settings/qsettingsapplicationstore.h"
#include "settings/applicationsettingsservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>

namespace
{
	int g_nFailureCount = 0;

	class KFailingApplicationSettingsStore final : public KApplicationSettingsStore
	{
	public:
		bool loadSettings(KApplicationSettings *pSettings, QString *) override
		{
			if (pSettings != nullptr)
				*pSettings = KApplicationSettings();
			return true;
		}

		bool saveSettings(const KApplicationSettings &, QString *pError) override
		{
			if (pError != nullptr)
				*pError = QStringLiteral("simulated write failure");
			return false;
		}
	};

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void testDefaultsAndPersistence()
	{
		QTemporaryDir temporaryDir;
		const QString strFilePath = temporaryDir.filePath(QStringLiteral("settings.ini"));
		KQSettingsApplicationStore store(strFilePath);
		KApplicationSettings loaded;
		QString strError;
		check(store.loadSettings(&loaded, &strError), QStringLiteral("missing settings use defaults"));
		check(loaded.bRemoteAccessEnabled
			&& loaded.approvalMode == AskRemoteApprovalMode
			&& loaded.nApprovalTimeoutSeconds == 30
			&& loaded.nDefaultListenPort == 39000
			&& loaded.strDefaultRole == QStringLiteral("controller"),
			QStringLiteral("safe application defaults are loaded"));

		loaded.bRemoteAccessEnabled = false;
		loaded.approvalMode = AutoAcceptRemoteApprovalMode;
		loaded.nApprovalTimeoutSeconds = 45;
		loaded.nDefaultListenPort = 40100;
		loaded.strDefaultRole = QStringLiteral("controlled");
		check(store.saveSettings(loaded, &strError), QStringLiteral("application settings save"));
		KApplicationSettings roundTrip;
		check(store.loadSettings(&roundTrip, &strError)
			&& !roundTrip.bRemoteAccessEnabled
			&& roundTrip.approvalMode == AutoAcceptRemoteApprovalMode
			&& roundTrip.nApprovalTimeoutSeconds == 45
			&& roundTrip.nDefaultListenPort == 40100
			&& roundTrip.strDefaultRole == QStringLiteral("controlled"),
			QStringLiteral("application settings round-trip"));
	}

	void testServiceValidation()
	{
		QTemporaryDir temporaryDir;
		auto spStore = std::make_unique<KQSettingsApplicationStore>(
			temporaryDir.filePath(QStringLiteral("settings.ini")));
		KApplicationSettingsService service(std::move(spStore));
		service.initialize();
		int nErrorCount = 0;
		int nChangedCount = 0;
		QObject::connect(&service, &KApplicationSettingsService::settingsError,
			[&nErrorCount](const QString &) { ++nErrorCount; });
		QObject::connect(&service, &KApplicationSettingsService::settingsChanged,
			[&nChangedCount](const KApplicationSettings &) { ++nChangedCount; });
		service.updateSettings(true, QStringLiteral("invalid"), 30, 39000,
			QStringLiteral("controller"));
		check(nErrorCount == 1 && nChangedCount == 0,
			QStringLiteral("invalid settings are rejected without publication"));
		service.updateSettings(true, QStringLiteral("deny"), 60, 39001,
			QStringLiteral("controlled"));
		check(nChangedCount == 1
			&& service.settings().approvalMode == DenyRemoteApprovalMode,
			QStringLiteral("valid settings are saved and published"));
	}

	void testInvalidStoredValuesUseSafeFallbacks()
	{
		QTemporaryDir temporaryDir;
		const QString strFilePath = temporaryDir.filePath(QStringLiteral("settings.ini"));
		QSettings rawSettings(strFilePath, QSettings::IniFormat);
		rawSettings.setValue(QStringLiteral("remoteAccess/approvalMode"), QStringLiteral("unknown"));
		rawSettings.setValue(QStringLiteral("remoteAccess/approvalTimeoutSeconds"), 999);
		rawSettings.setValue(QStringLiteral("connection/defaultListenPort"), 0);
		rawSettings.setValue(QStringLiteral("connection/defaultRole"), QStringLiteral("invalid"));
		rawSettings.sync();

		KQSettingsApplicationStore store(strFilePath);
		KApplicationSettings loaded;
		check(store.loadSettings(&loaded, nullptr)
			&& loaded.approvalMode == AskRemoteApprovalMode
			&& loaded.nApprovalTimeoutSeconds == 30
			&& loaded.nDefaultListenPort == 39000
			&& loaded.strDefaultRole == QStringLiteral("controller"),
			QStringLiteral("invalid stored values use bounded safe fallbacks"));
	}

	void testWriteFailureKeepsPreviousSettings()
	{
		auto spStore = std::make_unique<KFailingApplicationSettingsStore>();
		KApplicationSettingsService service(std::move(spStore));
		service.initialize();
		int nErrorCount = 0;
		int nChangedCount = 0;
		QObject::connect(&service, &KApplicationSettingsService::settingsError,
			[&nErrorCount](const QString &) { ++nErrorCount; });
		QObject::connect(&service, &KApplicationSettingsService::settingsChanged,
			[&nChangedCount](const KApplicationSettings &) { ++nChangedCount; });

		service.updateSettings(false, QStringLiteral("deny"), 60, 40100,
			QStringLiteral("controlled"));
		const KApplicationSettings settings = service.settings();
		check(nErrorCount == 1
			&& nChangedCount == 0
			&& settings.bRemoteAccessEnabled
			&& settings.approvalMode == AskRemoteApprovalMode
			&& settings.nApprovalTimeoutSeconds == 30
			&& settings.nDefaultListenPort == 39000
			&& settings.strDefaultRole == QStringLiteral("controller"),
			QStringLiteral("write failure keeps previous settings"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testDefaultsAndPersistence();
	testServiceValidation();
	testInvalidStoredValuesUseSafeFallbacks();
	testWriteFailureKeepsPreviousSettings();
	if (g_nFailureCount == 0)
		qInfo() << "All application settings tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
