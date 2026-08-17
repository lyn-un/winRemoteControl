#include "core/security/sourcefailuretracker.h"

#include <QtCore/QtGlobal>

KSourceFailureTracker::KSourceFailureTracker(int nMaximumFailures,
	qint64 nWindowMs,
	int nMaximumSources)
	: m_nMaximumFailures(qMax(1, nMaximumFailures))
	, m_nWindowMs(qMax<qint64>(1, nWindowMs))
	, m_nMaximumSources(qMax(1, nMaximumSources))
{
}

bool KSourceFailureTracker::isRateLimited(const QString &strSource,
	qint64 nNowMs)
{
	if (strSource.isEmpty())
		return false;
	prune(nNowMs);
	const auto iterator = m_failures.constFind(strSource);
	return iterator != m_failures.constEnd()
		&& iterator->size() >= m_nMaximumFailures;
}

void KSourceFailureTracker::recordFailure(const QString &strSource,
	qint64 nNowMs)
{
	if (strSource.isEmpty())
		return;
	prune(nNowMs);
	if (!m_failures.contains(strSource)
		&& m_failures.size() >= m_nMaximumSources)
	{
		evictOldestSource(nNowMs);
	}
	QVector<qint64> &failures = m_failures[strSource];
	if (failures.size() < m_nMaximumFailures)
		failures.append(nNowMs);
}

int KSourceFailureTracker::trackedSourceCount() const
{
	return m_failures.size();
}

void KSourceFailureTracker::clear()
{
	m_failures.clear();
}

void KSourceFailureTracker::prune(qint64 nNowMs)
{
	const qint64 nCutoffMs = nNowMs - m_nWindowMs;
	for (auto iterator = m_failures.begin(); iterator != m_failures.end();)
	{
		QVector<qint64> &failures = iterator.value();
		while (!failures.isEmpty() && failures.first() <= nCutoffMs)
			failures.removeFirst();
		if (failures.isEmpty())
			iterator = m_failures.erase(iterator);
		else
			++iterator;
	}
}

void KSourceFailureTracker::evictOldestSource(qint64 nNowMs)
{
	QString strOldestSource;
	qint64 nOldestFailureMs = nNowMs;
	for (auto iterator = m_failures.constBegin();
		iterator != m_failures.constEnd(); ++iterator)
	{
		if (!iterator->isEmpty() && iterator->last() <= nOldestFailureMs)
		{
			nOldestFailureMs = iterator->last();
			strOldestSource = iterator.key();
		}
	}
	if (!strOldestSource.isEmpty())
		m_failures.remove(strOldestSource);
}
