#ifndef _WINREMOTECONTROL_CORE_SETTINGS_DEVICESECURITYPREFERENCE_H_
#define _WINREMOTECONTROL_CORE_SETTINGS_DEVICESECURITYPREFERENCE_H_

#include "core/privacy/privacytypes.h"

#include <QtCore/QMetaType>
#include <QtCore/QString>

struct KDeviceSecurityPreference
{
	QString strRemoteDeviceId;
	KPrivacyMode desiredPrivacyMode = DisabledPrivacyMode;
	KPostSessionAction desiredPostSessionAction = NoPostSessionAction;
	qint64 nUpdatedAtMs = 0;
};

QString DeviceSecurityPrivacyModeName(KPrivacyMode mode);
bool DeviceSecurityPrivacyModeFromName(const QString &strName, KPrivacyMode *pMode);
QString DeviceSecurityPostSessionActionName(KPostSessionAction action);
bool DeviceSecurityPostSessionActionFromName(const QString &strName,
	KPostSessionAction *pAction);

Q_DECLARE_METATYPE(KDeviceSecurityPreference)

#endif // _WINREMOTECONTROL_CORE_SETTINGS_DEVICESECURITYPREFERENCE_H_
