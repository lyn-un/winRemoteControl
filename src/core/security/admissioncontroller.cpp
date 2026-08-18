#include "core/security/admissioncontroller.h"

#include <QtCore/QDateTime>

bool KAdmissionController::isRateLimited(const QString &strSourceAddress,
	qint64 nNowMs)
{
	if (nNowMs < 0)
		nNowMs = QDateTime::currentMSecsSinceEpoch();
	return m_failureTracker.isRateLimited(
		normalizedSourceAddress(strSourceAddress), nNowMs);
}

void KAdmissionController::recordPeerFailure(
	const QString &strSourceAddress,
	qint64 nNowMs)
{
	if (nNowMs < 0)
		nNowMs = QDateTime::currentMSecsSinceEpoch();
	m_failureTracker.recordFailure(normalizedSourceAddress(strSourceAddress),
		nNowMs);
}

qsizetype KAdmissionController::trackedSourceCount() const
{
	return m_failureTracker.trackedSourceCount();
}

QString KAdmissionController::normalizedSourceAddress(
	const QString &strSourceAddress)
{
	return strSourceAddress.trimmed().toLower();
}
