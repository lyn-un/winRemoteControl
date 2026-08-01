#include "adapters/settings/qsettingsapplicationstore.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>

KQSettingsApplicationStore::KQSettingsApplicationStore(const QString &strFilePath)
	: m_strFilePath(strFilePath)
{
}

KQSettingsApplicationStore::~KQSettingsApplicationStore()
{
}

bool KQSettingsApplicationStore::loadSettings(KApplicationSettings *pSettings, QString *pError)
{
	if (pSettings == nullptr)
		return false;

	KApplicationSettings settings;
	QSettings storage(m_strFilePath, QSettings::IniFormat);
	settings.bRemoteAccessEnabled = storage.value(
		QStringLiteral("remoteAccess/enabled"), settings.bRemoteAccessEnabled).toBool();
	const QString strApprovalMode = storage.value(
		QStringLiteral("remoteAccess/approvalMode"), QStringLiteral("ask")).toString();
	if (!RemoteApprovalModeFromName(strApprovalMode, &settings.approvalMode))
		settings.approvalMode = AskRemoteApprovalMode;
	settings.nApprovalTimeoutSeconds = storage.value(
		QStringLiteral("remoteAccess/approvalTimeoutSeconds"),
		settings.nApprovalTimeoutSeconds).toInt();
	settings.nDefaultListenPort = static_cast<quint16>(storage.value(
		QStringLiteral("connection/defaultListenPort"),
		settings.nDefaultListenPort).toUInt());
	settings.strDefaultRole = storage.value(
		QStringLiteral("connection/defaultRole"), settings.strDefaultRole).toString();
	if (storage.status() != QSettings::NoError)
	{
		if (pError != nullptr)
			*pError = QStringLiteral("读取应用设置失败");
		return false;
	}

	*pSettings = SanitizeApplicationSettings(settings);
	return true;
}

bool KQSettingsApplicationStore::saveSettings(
	const KApplicationSettings &settings,
	QString *pError)
{
	const QFileInfo fileInfo(m_strFilePath);
	if (!QDir().mkpath(fileInfo.absolutePath()))
	{
		if (pError != nullptr)
			*pError = QStringLiteral("无法创建应用设置目录");
		return false;
	}

	QSettings storage(m_strFilePath, QSettings::IniFormat);
	storage.setValue(QStringLiteral("remoteAccess/enabled"), settings.bRemoteAccessEnabled);
	storage.setValue(QStringLiteral("remoteAccess/approvalMode"),
		RemoteApprovalModeName(settings.approvalMode));
	storage.setValue(QStringLiteral("remoteAccess/approvalTimeoutSeconds"),
		settings.nApprovalTimeoutSeconds);
	storage.setValue(QStringLiteral("connection/defaultListenPort"),
		settings.nDefaultListenPort);
	storage.setValue(QStringLiteral("connection/defaultRole"), settings.strDefaultRole);
	storage.sync();
	if (storage.status() == QSettings::NoError)
		return true;
	if (pError != nullptr)
		*pError = QStringLiteral("写入应用设置失败");
	return false;
}
