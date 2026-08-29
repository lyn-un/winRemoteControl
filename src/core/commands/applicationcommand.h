#ifndef _WINREMOTECONTROL_APPLICATIONCOMMAND_H_
#define _WINREMOTECONTROL_APPLICATIONCOMMAND_H_

#include "core/commands/applicationcommandresult.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <functional>

struct KApplicationCommand
{
	QString strId;
	std::function<bool()> canExecute;
	std::function<KApplicationCommandResult(const QJsonObject &)> execute;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMMAND_H_
