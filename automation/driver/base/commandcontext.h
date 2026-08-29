#ifndef _WINREMOTECONTROL_DRIVER_COMMANDCONTEXT_H_
#define _WINREMOTECONTROL_DRIVER_COMMANDCONTEXT_H_

#include <QtCore/QJsonObject>
#include <QtCore/QHash>
#include <QtCore/QString>

#include <functional>

struct KDriverCommandContext
{
	quint64 nRequestId = 0;
	QString strSessionId;
	QHash<QString, QString> pathParameters;
	QJsonObject parameters;
	QJsonObject result;
	std::function<void(const QJsonObject &)> completed;
	bool bCompleted = false;
	bool bTimedOut = false;

	bool complete(const QJsonObject &response);
	bool timeout(const QJsonObject &response);
};

#endif // _WINREMOTECONTROL_DRIVER_COMMANDCONTEXT_H_
