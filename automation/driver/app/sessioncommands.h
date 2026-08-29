#ifndef _WINREMOTECONTROL_DRIVER_SESSIONCOMMANDS_H_
#define _WINREMOTECONTROL_DRIVER_SESSIONCOMMANDS_H_

#include "automation/driver/base/session.h"

#include <QtCore/QJsonObject>

QJsonObject DriverStatusValue(const QString &strPid, const QString &strBuildId, bool bReady);
QJsonObject DriverSessionValue(const KDriverSession &session,
	const QString &strPid,
	const QString &strBuildId);

#endif // _WINREMOTECONTROL_DRIVER_SESSIONCOMMANDS_H_
