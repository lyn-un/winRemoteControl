#include "automation/driver/base/sessionmanager.h"

#include <QtCore/QUuid>

KDriverSession KDriverSessionManager::createSession(qint64 nNowMs)
{
	KDriverSession session;
	session.strSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	session.nCreatedAtMs = nNowMs;
	session.nLastActivityAtMs = nNowMs;
	m_sessions.insert(session.strSessionId, session);
	return session;
}

KDriverSession *KDriverSessionManager::session(const QString &strSessionId, qint64 nNowMs)
{
	auto iter = m_sessions.find(strSessionId);
	if (iter == m_sessions.end() || !iter->isValid())
		return nullptr;
	iter->touch(nNowMs);
	return &iter.value();
}

bool KDriverSessionManager::quitSession(const QString &strSessionId)
{
	auto iter = m_sessions.find(strSessionId);
	if (iter == m_sessions.end())
		return false;
	iter->bQuit = true;
	m_sessions.erase(iter);
	return true;
}

int KDriverSessionManager::collectExpired(qint64 nNowMs, qint64 nMaximumIdleMs)
{
	int nRemoved = 0;
	for (auto iter = m_sessions.begin(); iter != m_sessions.end();)
	{
		if (iter->bQuit || nNowMs - iter->nLastActivityAtMs > nMaximumIdleMs)
		{
			iter = m_sessions.erase(iter);
			++nRemoved;
		}
		else
		{
			++iter;
		}
	}
	return nRemoved;
}

void KDriverSessionManager::clear()
{
	m_sessions.clear();
}

int KDriverSessionManager::sessionCount() const
{
	return m_sessions.size();
}
