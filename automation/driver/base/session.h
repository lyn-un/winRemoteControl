#ifndef _WINREMOTECONTROL_DRIVER_SESSION_H_
#define _WINREMOTECONTROL_DRIVER_SESSION_H_

#include <QtCore/QString>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>

struct KDriverIdempotencyRecord
{
	QString strCommandId;
	QJsonObject arguments;
	QJsonObject response;
	bool bCompleted = false;
};

struct KDriverSession
{
	QString strSessionId;
	qint64 nCreatedAtMs = 0;
	qint64 nLastActivityAtMs = 0;
	quint64 nEventCursor = 0;
	quint64 nSessionGeneration = 0;
	QHash<QString, KDriverIdempotencyRecord> idempotencyRecords;
	bool bQuit = false;

	bool isValid() const;
	void touch(qint64 nNowMs);
};

#endif // _WINREMOTECONTROL_DRIVER_SESSION_H_
