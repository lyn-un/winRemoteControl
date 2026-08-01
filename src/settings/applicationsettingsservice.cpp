#include "settings/applicationsettingsservice.h"

#include "common/sessiontracelogger.h"
#include "core/settings/applicationsettingsstore.h"

#include <utility>

KApplicationSettingsService::KApplicationSettingsService(
	std::unique_ptr<KApplicationSettingsStore> spStore,
	QObject *pParent)
	: QObject(pParent)
	, m_spStore(std::move(spStore))
{
	Q_ASSERT(m_spStore != nullptr);
}

KApplicationSettingsService::~KApplicationSettingsService()
{
}

void KApplicationSettingsService::initialize()
{
	KApplicationSettings loadedSettings;
	QString strError;
	if (!m_spStore->loadSettings(&loadedSettings, &strError))
	{
		emit settingsError(strError);
		return;
	}
	m_settings = SanitizeApplicationSettings(loadedSettings);
}

KApplicationSettings KApplicationSettingsService::settings() const
{
	return m_settings;
}

void KApplicationSettingsService::requestSettings()
{
	emit settingsChanged(m_settings);
}

void KApplicationSettingsService::updateSettings(bool bRemoteAccessEnabled,
	const QString &strApprovalMode,
	int nApprovalTimeoutSeconds,
	int nDefaultListenPort,
	const QString &strDefaultRole)
{
	KRemoteApprovalMode approvalMode;
	if (!RemoteApprovalModeFromName(strApprovalMode, &approvalMode)
		|| nApprovalTimeoutSeconds < 10
		|| nApprovalTimeoutSeconds > 120
		|| nDefaultListenPort <= 0
		|| nDefaultListenPort > 65535
		|| (strDefaultRole != QStringLiteral("controller")
			&& strDefaultRole != QStringLiteral("controlled")))
	{
		emit settingsError(QStringLiteral("应用设置参数无效"));
		return;
	}

	KApplicationSettings candidate;
	candidate.bRemoteAccessEnabled = bRemoteAccessEnabled;
	candidate.approvalMode = approvalMode;
	candidate.nApprovalTimeoutSeconds = nApprovalTimeoutSeconds;
	candidate.nDefaultListenPort = static_cast<quint16>(nDefaultListenPort);
	candidate.strDefaultRole = strDefaultRole;

	QString strError;
	if (!m_spStore->saveSettings(candidate, &strError))
	{
		emit settingsError(strError);
		return;
	}

	m_settings = candidate;
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("settings"),
		QStringLiteral("updated"),
		-1,
		QStringLiteral("remoteAccess=%1 approvalMode=%2 timeoutSeconds=%3 listenPort=%4 defaultRole=%5")
			.arg(m_settings.bRemoteAccessEnabled ? 1 : 0)
			.arg(RemoteApprovalModeName(m_settings.approvalMode))
			.arg(m_settings.nApprovalTimeoutSeconds)
			.arg(m_settings.nDefaultListenPort)
			.arg(m_settings.strDefaultRole));
	emit settingsChanged(m_settings);
}
