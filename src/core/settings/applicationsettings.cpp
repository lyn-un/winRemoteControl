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
	if (sanitized.strDefaultRole != QStringLiteral("controller")
		&& sanitized.strDefaultRole != QStringLiteral("controlled"))
	{
		sanitized.strDefaultRole = QStringLiteral("controller");
	}
	return sanitized;
}
