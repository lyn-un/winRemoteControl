#ifndef _WINREMOTECONTROL_APPLICATIONCOMMANDRESULT_H_
#define _WINREMOTECONTROL_APPLICATIONCOMMANDRESULT_H_

#include <QtCore/QJsonValue>
#include <QtCore/QString>

enum KApplicationCommandStatus
{
	ApplicationCommandSucceeded,
	ApplicationCommandNotFound,
	ApplicationCommandDisabled,
	ApplicationCommandInvalidArgument,
	ApplicationCommandBusy,
	ApplicationCommandFailed
};

struct KApplicationCommandResult
{
	KApplicationCommandStatus status = ApplicationCommandFailed;
	QJsonValue value;
	QString strErrorCode;
	QString strTechnicalMessage;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMMANDRESULT_H_
