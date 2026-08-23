#ifndef _WINREMOTECONTROL_CORE_SETTINGS_APPLICATIONSETTINGS_H_
#define _WINREMOTECONTROL_CORE_SETTINGS_APPLICATIONSETTINGS_H_

#include <QtCore/QMetaType>
#include <QtCore/QString>

enum KRemoteApprovalMode
{
	AskRemoteApprovalMode,
	AutoAcceptRemoteApprovalMode,
	DenyRemoteApprovalMode
};

struct KApplicationSettings
{
	bool bRemoteAccessEnabled = true;
	KRemoteApprovalMode approvalMode = AskRemoteApprovalMode;
	int nApprovalTimeoutSeconds = 30;
	quint16 nDefaultListenPort = 39000;
	QString strThemeId = QStringLiteral("nordic-mist");
};

QString RemoteApprovalModeName(KRemoteApprovalMode mode);
bool RemoteApprovalModeFromName(const QString &strName, KRemoteApprovalMode *pMode);
QString DefaultApplicationThemeId();
bool IsApplicationThemeIdValid(const QString &strThemeId);
KApplicationSettings SanitizeApplicationSettings(const KApplicationSettings &settings);

Q_DECLARE_METATYPE(KApplicationSettings)

#endif // _WINREMOTECONTROL_CORE_SETTINGS_APPLICATIONSETTINGS_H_
