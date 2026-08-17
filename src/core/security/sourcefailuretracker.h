#ifndef _WINREMOTECONTROL_CORE_SECURITY_SOURCEFAILURETRACKER_H_
#define _WINREMOTECONTROL_CORE_SECURITY_SOURCEFAILURETRACKER_H_

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QVector>

class KSourceFailureTracker
{
public:
	KSourceFailureTracker(int nMaximumFailures = 5,
		qint64 nWindowMs = 10 * 60 * 1000LL,
		int nMaximumSources = 256);

	bool isRateLimited(const QString &strSource, qint64 nNowMs);
	void recordFailure(const QString &strSource, qint64 nNowMs);
	int trackedSourceCount() const;
	void clear();

private:
	void prune(qint64 nNowMs);
	void evictOldestSource(qint64 nNowMs);

	int m_nMaximumFailures = 5;
	qint64 m_nWindowMs = 10 * 60 * 1000LL;
	int m_nMaximumSources = 256;
	QHash<QString, QVector<qint64>> m_failures;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_SOURCEFAILURETRACKER_H_
