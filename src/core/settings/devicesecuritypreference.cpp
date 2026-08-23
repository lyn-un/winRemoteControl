#include "core/settings/devicesecuritypreference.h"

QString DeviceSecurityPrivacyModeName(KPrivacyMode mode)
{
	if (mode == PrivacyOverlayPrivacyMode)
		return QStringLiteral("privacyoverlay");
	if (mode == DisplayOffPrivacyMode)
		return QStringLiteral("displayoff");
	return QStringLiteral("disabled");
}

bool DeviceSecurityPrivacyModeFromName(const QString &strName, KPrivacyMode *pMode)
{
	if (pMode == nullptr)
		return false;
	if (strName == QStringLiteral("disabled"))
		*pMode = DisabledPrivacyMode;
	else if (strName == QStringLiteral("privacyoverlay"))
		*pMode = PrivacyOverlayPrivacyMode;
	else if (strName == QStringLiteral("displayoff"))
		*pMode = DisplayOffPrivacyMode;
	else
		return false;
	return true;
}

QString DeviceSecurityPostSessionActionName(KPostSessionAction action)
{
	return action == LockWorkstationPostSessionAction
		? QStringLiteral("lockworkstation") : QStringLiteral("none");
}

bool DeviceSecurityPostSessionActionFromName(const QString &strName,
	KPostSessionAction *pAction)
{
	if (pAction == nullptr)
		return false;
	if (strName == QStringLiteral("none"))
		*pAction = NoPostSessionAction;
	else if (strName == QStringLiteral("lockworkstation"))
		*pAction = LockWorkstationPostSessionAction;
	else
		return false;
	return true;
}
