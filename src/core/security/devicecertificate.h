#ifndef _WINREMOTECONTROL_CORE_SECURITY_DEVICECERTIFICATE_H_
#define _WINREMOTECONTROL_CORE_SECURITY_DEVICECERTIFICATE_H_

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QString>

struct KDeviceCertificate
{
	QString strDeviceId;
	QByteArray certificateDer;
	QByteArray spkiSha256;
	QByteArray certificateSha256;
	QDateTime validFromUtc;
	QDateTime validToUtc;

	bool isValid() const;
	QString spkiFingerprint() const;
};

#endif // _WINREMOTECONTROL_CORE_SECURITY_DEVICECERTIFICATE_H_
