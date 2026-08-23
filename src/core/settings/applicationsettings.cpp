#include "core/settings/applicationsettings.h"

QString RemoteApprovalModeName(KRemoteApprovalMode mode)
{
	if (mode == AutoAcceptRemoteApprovalMode)
		return QStringLiteral("autoAccept");
	if (mode == DenyRemoteApprovalMode)
		return QStringLiteral("deny");
	return QStringLiteral("ask");
}

bool RemoteApprovalModeFromName(const QString &strName, KRemoteApprovalMode *pMode)
{
	if (pMode == nullptr)
		return false;
	if (strName == QStringLiteral("ask"))
		*pMode = AskRemoteApprovalMode;
	else if (strName == QStringLiteral("autoAccept"))
		*pMode = AutoAcceptRemoteApprovalMode;
	else if (strName == QStringLiteral("deny"))
		*pMode = DenyRemoteApprovalMode;
	else
		return false;
	return true;
}

QString DefaultApplicationThemeId()
{
	return QStringLiteral("nordic-mist");
}

bool IsApplicationThemeIdValid(const QString &strThemeId)
{
	return strThemeId == QStringLiteral("daylight")
		|| strThemeId == QStringLiteral("midnight")
		|| strThemeId == DefaultApplicationThemeId();
}

KApplicationSettings SanitizeApplicationSettings(const KApplicationSettings &settings)
{
	KApplicationSettings sanitized = settings;
	if (sanitized.approvalMode != AskRemoteApprovalMode
		&& sanitized.approvalMode != AutoAcceptRemoteApprovalMode
		&& sanitized.approvalMode != DenyRemoteApprovalMode)
	{
		sanitized.approvalMode = AskRemoteApprovalMode;
	}
	if (sanitized.nApprovalTimeoutSeconds < 10 || sanitized.nApprovalTimeoutSeconds > 120)
		sanitized.nApprovalTimeoutSeconds = 30;
	if (sanitized.nDefaultListenPort == 0)
		sanitized.nDefaultListenPort = 39000;
	if (!IsApplicationThemeIdValid(sanitized.strThemeId))
		sanitized.strThemeId = DefaultApplicationThemeId();
	return sanitized;
}
