#ifndef _WINREMOTECONTROL_CORE_SECURITY_DEVICEIDENTITY_H_
#define _WINREMOTECONTROL_CORE_SECURITY_DEVICEIDENTITY_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

struct KDeviceIdentity
{
	QString strDeviceId;
	QString strAlgorithm;
	QByteArray publicKey;
	QString strFingerprint;

	bool isValid() const;
};

QString DevicePublicKeyFingerprint(const QByteArray &publicKey);

#endif // _WINREMOTECONTROL_CORE_SECURITY_DEVICEIDENTITY_H_
