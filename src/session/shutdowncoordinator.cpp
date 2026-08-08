#include "session/shutdowncoordinator.h"

#include <QtCore/QTimer>

KShutdownCoordinator::KShutdownCoordinator(QObject *pParent)
	: QObject(pParent)
	, m_pWatchdogTimer(new QTimer(this))
{
	m_pWatchdogTimer->setSingleShot(true);
	connect(m_pWatchdogTimer, &QTimer::timeout, this,
		[this]()
		{
			emit watchdogExpired(m_nGeneration,
				m_bCapturePending, m_bPeerPending);
		});
}

void KShutdownCoordinator::begin(quint64 nGeneration,
	bool bCapturePending,
	bool bPeerPending,
	int nWatchdogMs)
{
	clear();
	m_nGeneration = nGeneration;
	m_bActive = true;
	m_bCapturePending = bCapturePending;
	m_bPeerPending = bPeerPending;
	m_pWatchdogTimer->start(qMax(1, nWatchdogMs));
	tryFinish();
}

void KShutdownCoordinator::completeCapture(quint64 nGeneration)
{
	if (!m_bActive || nGeneration != m_nGeneration)
		return;
	m_bCapturePending = false;
	tryFinish();
}

void KShutdownCoordinator::completePeer(quint64 nGeneration)
{
	if (!m_bActive || nGeneration != m_nGeneration)
		return;
	m_bPeerPending = false;
	tryFinish();
}

void KShutdownCoordinator::clear()
{
	m_pWatchdogTimer->stop();
	m_nGeneration = 0;
	m_bActive = false;
	m_bCapturePending = false;
	m_bPeerPending = false;
}

bool KShutdownCoordinator::isActive() const
{
	return m_bActive;
}

bool KShutdownCoordinator::isCapturePending() const
{
	return m_bCapturePending;
}

bool KShutdownCoordinator::isPeerPending() const
{
	return m_bPeerPending;
}

quint64 KShutdownCoordinator::generation() const
{
	return m_nGeneration;
}

void KShutdownCoordinator::tryFinish()
{
	if (!m_bActive || m_bCapturePending || m_bPeerPending)
		return;
	const quint64 nGeneration = m_nGeneration;
	clear();
	emit finished(nGeneration);
}
