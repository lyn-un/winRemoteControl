#ifndef _WINREMOTECONTROL_DRIVER_SESSIONMANAGER_H_
#define _WINREMOTECONTROL_DRIVER_SESSIONMANAGER_H_

#include "automation/driver/base/session.h"

#include <QtCore/QHash>
#include <QtCore/QList>

class KDriverSessionManager
{
public:
	KDriverSession createSession(qint64 nNowMs,
		quint64 nEventCursor,
		quint64 nSessionGeneration);
	KDriverSession *session(const QString &strSessionId, qint64 nNowMs);
	bool quitSession(const QString &strSessionId);
	int collectExpired(qint64 nNowMs, qint64 nMaximumIdleMs);
	void clear();
	int sessionCount() const;

private:
	QHash<QString, KDriverSession> m_sessions;
};

#endif // _WINREMOTECONTROL_DRIVER_SESSIONMANAGER_H_
