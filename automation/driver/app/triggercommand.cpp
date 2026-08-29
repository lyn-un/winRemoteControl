#include "automation/driver/app/triggercommand.h"

#include "automation/driver/base/status.h"

namespace
{
	KDriverStatus ApplicationStatusToDriverStatus(int nStatus)
	{
		if (nStatus == 1)
			return UnknownCommandDriverStatus;
		if (nStatus == 2)
			return CommandDisabledDriverStatus;
		if (nStatus == 3)
			return InvalidArgumentDriverStatus;
		if (nStatus == 4)
			return CommandBusyDriverStatus;
		if (nStatus == 0)
			return OkDriverStatus;
		return InternalErrorDriverStatus;
	}
}

QJsonObject MapApplicationCommandResponse(const QJsonObject &hostResponse)
{
	const KDriverStatus status = ApplicationStatusToDriverStatus(
		hostResponse.value(QStringLiteral("status")).toInt(5));
	if (status == OkDriverStatus)
		return DriverSuccessResponse(hostResponse.value(QStringLiteral("value")));
	return DriverErrorResponse(status,
		hostResponse.value(QStringLiteral("errorCode")).toString(),
		hostResponse.value(QStringLiteral("technicalMessage")).toString());
}
