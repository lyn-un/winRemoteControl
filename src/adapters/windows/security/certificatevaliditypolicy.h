#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_CERTIFICATEVALIDITYPOLICY_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_CERTIFICATEVALIDITYPOLICY_H_

#include <QtCore/QDateTime>

struct KCertificateValidityPeriod
{
	QDateTime validFromUtc;
	QDateTime validToUtc;
};

KCertificateValidityPeriod BuildDeviceCertificateValidityPeriod(
	const QDateTime &currentUtc);

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_SECURITY_CERTIFICATEVALIDITYPOLICY_H_
