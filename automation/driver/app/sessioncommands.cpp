#include "automation/driver/app/sessioncommands.h"

QJsonObject DriverStatusValue(const QString &strPid, const QString &strBuildId, bool bReady)
{
	return QJsonObject{
		{QStringLiteral("ready"), bReady},
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
		{QStringLiteral("pid"), strPid},
		{QStringLiteral("protocolVersion"), 1},
		{QStringLiteral("buildId"), strBuildId}
	};
}
