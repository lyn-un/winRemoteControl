#ifndef _WINREMOTECONTROL_DRIVER_REQUESTPARSER_H_
#define _WINREMOTECONTROL_DRIVER_REQUESTPARSER_H_

#include "automation/driver/base/status.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

struct KParsedDriverRequest
{
	QString strMethod;
	QString strPath;
	QJsonObject parameters;
};

class KRequestParser
{
public:
	static constexpr int kMaximumBodyBytes = 16 * 1024;
	static constexpr int kMaximumCommandIdBytes = 128;

	static bool parse(const QByteArray &methodUtf8,
		const QByteArray &pathUtf8,
		const QByteArray &bodyUtf8,
		KParsedDriverRequest *pRequest,
		QJsonObject *pErrorResponse);
};

#endif // _WINREMOTECONTROL_DRIVER_REQUESTPARSER_H_
