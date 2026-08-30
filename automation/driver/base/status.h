#ifndef _WINREMOTECONTROL_DRIVER_STATUS_H_
#define _WINREMOTECONTROL_DRIVER_STATUS_H_

#include <QtCore/QJsonObject>
#include <QtCore/QString>

enum KDriverStatus
{
	OkDriverStatus = 0,
	InvalidArgumentDriverStatus,
	InvalidSessionIdDriverStatus,
	UnknownCommandDriverStatus,
	CommandDisabledDriverStatus,
	CommandBusyDriverStatus,
	CommandTimeoutDriverStatus,
	CommandExecutionStartedDriverStatus,
	UnsupportedOperationDriverStatus,
	InternalErrorDriverStatus
};

QString DriverStatusName(KDriverStatus status);
QJsonObject DriverSuccessResponse(const QJsonValue &value);
QJsonObject DriverErrorResponse(KDriverStatus status,
	const QString &strError,
	const QString &strMessage);
QJsonObject DriverErrorResponse(KDriverStatus status,
	const QString &strError,
	const QString &strMessage,
	bool bRetryable,
	bool bOutcomeUnknown);

#endif // _WINREMOTECONTROL_DRIVER_STATUS_H_
