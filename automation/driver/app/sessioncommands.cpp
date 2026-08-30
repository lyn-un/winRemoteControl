#include "automation/driver/app/sessioncommands.h"

QJsonObject DriverStatusValue(const QString &strPid,
	const QString &strBuildId,
	bool bDriverReady,
	bool bHostReady)
{
	return QJsonObject{
		{QStringLiteral("ready"), bDriverReady && bHostReady},
		{QStringLiteral("driverReady"), bDriverReady},
		{QStringLiteral("hostReady"), bHostReady},
		{QStringLiteral("pid"), strPid},
		{QStringLiteral("protocolVersion"), 1},
		{QStringLiteral("buildId"), strBuildId}
	};
}

QJsonObject DriverSessionValue(const KDriverSession &session,
	const QString &strPid,
	const QString &strBuildId)
{
	return QJsonObject{
		{QStringLiteral("sessionId"), session.strSessionId},
		{QStringLiteral("eventCursor"), QString::number(session.nEventCursor)},
		{QStringLiteral("sessionGeneration"),
			QString::number(session.nSessionGeneration)},
		{QStringLiteral("pid"), strPid},
		{QStringLiteral("protocolVersion"), 1},
		{QStringLiteral("buildId"), strBuildId}
	};
}
