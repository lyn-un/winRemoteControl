#include "session/recoverycontroller.h"

#include <QtCore/QTimer>

KRecoveryController::KRecoveryController(QObject *pParent)
	: QObject(pParent)
	, m_pTimer(new QTimer(this))
{
	m_pTimer->setSingleShot(true);
	connect(m_pTimer, &QTimer::timeout, this,
		[this]() { emit timedOut(m_nGeneration); });
}

void KRecoveryController::begin(quint64 nGeneration, int nTimeoutMs)
{
	clear();
	m_nGeneration = nGeneration;
	m_elapsedTimer.start();
	m_pTimer->start(qMax(1, nTimeoutMs));
}

qint64 KRecoveryController::complete(quint64 nGeneration)
{
	if (!isActive(nGeneration))
		return -1;
	return clear();
}

qint64 KRecoveryController::clear()
{
	const qint64 nElapsedMs = elapsedMs();
	m_pTimer->stop();
	m_elapsedTimer.invalidate();
	m_nGeneration = 0;
	return nElapsedMs;
}

bool KRecoveryController::isActive(quint64 nGeneration) const
{
	return m_elapsedTimer.isValid() && nGeneration == m_nGeneration;
}

qint64 KRecoveryController::elapsedMs() const
{
	return m_elapsedTimer.isValid() ? m_elapsedTimer.elapsed() : -1;
}

quint64 KRecoveryController::generation() const
{
	return m_nGeneration;
}
