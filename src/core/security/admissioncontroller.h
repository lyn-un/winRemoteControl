#ifndef _WINREMOTECONTROL_CORE_SECURITY_ADMISSIONCONTROLLER_H_
#define _WINREMOTECONTROL_CORE_SECURITY_ADMISSIONCONTROLLER_H_

#include "core/security/sourcefailuretracker.h"

class KAdmissionController
{
public:
	bool isRateLimited(const QString &strSourceAddress,
		qint64 nNowMs = -1);
	void recordPeerFailure(const QString &strSourceAddress,
		qint64 nNowMs = -1);
	qsizetype trackedSourceCount() const;

	static QString normalizedSourceAddress(const QString &strSourceAddress);

private:
	KSourceFailureTracker m_failureTracker;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_ADMISSIONCONTROLLER_H_
