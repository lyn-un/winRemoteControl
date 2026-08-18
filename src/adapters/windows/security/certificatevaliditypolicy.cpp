#include "adapters/windows/security/certificatevaliditypolicy.h"

namespace
{
	constexpr qint64 kCertificateClockSkewSeconds = 5 * 60;
	constexpr int kCertificateLifetimeYears = 5;
}

KCertificateValidityPeriod BuildDeviceCertificateValidityPeriod(
	const QDateTime &currentUtc)
{
	const QDateTime normalizedUtc = currentUtc.toUTC();
	KCertificateValidityPeriod period;
	period.validFromUtc = normalizedUtc.addSecs(-kCertificateClockSkewSeconds);
	period.validToUtc = normalizedUtc.addYears(kCertificateLifetimeYears);
	return period;
}
