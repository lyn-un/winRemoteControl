#ifndef _WINREMOTECONTROL_CORE_SECURITY_DEVICEIDENTITYPROVIDER_H_
#define _WINREMOTECONTROL_CORE_SECURITY_DEVICEIDENTITYPROVIDER_H_

#include "core/security/deviceidentity.h"
#include "core/security/devicecertificate.h"

class KDeviceIdentityProvider
{
public:
	virtual ~KDeviceIdentityProvider() = default;

	virtual bool initialize(QString *pErrorMessage) = 0;
	virtual KDeviceIdentity identity() const = 0;
	virtual bool sign(const QByteArray &data,
		QByteArray *pSignature,
		QString *pErrorMessage) const = 0;
	virtual bool verify(const QByteArray &publicKey,
		const QByteArray &data,
		const QByteArray &signature,
		QString *pErrorMessage) const = 0;
	virtual QByteArray randomBytes(int nByteCount,
		QString *pErrorMessage) const = 0;
	virtual KDeviceCertificate certificate() const = 0;
	virtual void *duplicateNativeCertificate(QString *pErrorMessage) const = 0;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_DEVICEIDENTITYPROVIDER_H_
