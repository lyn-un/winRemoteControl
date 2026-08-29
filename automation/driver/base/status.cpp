#include "automation/driver/base/status.h"

QString DriverStatusName(KDriverStatus status)
{
	if (status == OkDriverStatus)
		return QStringLiteral("ok");
	if (status == InvalidArgumentDriverStatus)
		return QStringLiteral("invalid_argument");
	if (status == InvalidSessionIdDriverStatus)
		return QStringLiteral("invalid_session_id");
	if (status == UnknownCommandDriverStatus)
		return QStringLiteral("unknown_command");
	if (status == CommandDisabledDriverStatus)
		return QStringLiteral("command_disabled");
	if (status == CommandBusyDriverStatus)
		return QStringLiteral("command_busy");
	if (status == CommandTimeoutDriverStatus)
		return QStringLiteral("command_timeout");
	if (status == UnsupportedOperationDriverStatus)
		return QStringLiteral("unsupported_operation");
	return QStringLiteral("internal_error");
}

QJsonObject DriverSuccessResponse(const QJsonValue &value)
{
	return QJsonObject{
		{QStringLiteral("status"), static_cast<int>(OkDriverStatus)},
		{QStringLiteral("value"), value},
		{QStringLiteral("isSuccess"), true}
	};
}

QJsonObject DriverErrorResponse(KDriverStatus status,
	const QString &strError,
	const QString &strMessage)
{
	return QJsonObject{
		{QStringLiteral("status"), static_cast<int>(status)},
		{QStringLiteral("value"), QJsonObject{
			{QStringLiteral("error"), strError.isEmpty() ? DriverStatusName(status) : strError},
			{QStringLiteral("message"), strMessage},
			{QStringLiteral("stacktrace"), QString()}
		}},
		{QStringLiteral("isSuccess"), false}
	};
}
