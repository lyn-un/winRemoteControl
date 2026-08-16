#ifndef _WINREMOTECONTROL_CORE_TRANSPORT_TLSPEERIDENTITY_H_
#define _WINREMOTECONTROL_CORE_TRANSPORT_TLSPEERIDENTITY_H_

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QMetaType>
#include <QtCore/QString>

struct KTlsPeerIdentity
{
	QString strDeviceId;
	QString strSourceAddress;
	QByteArray spkiSha256;
	QByteArray certificateSha256;
	QDateTime validFromUtc;
	QDateTime validToUtc;
	QString strTlsProtocol;
	QString strCipherSuite;

	bool isValid() const;
	QString spkiFingerprint() const;
};

Q_DECLARE_METATYPE(KTlsPeerIdentity)

#endif // _WINREMOTECONTROL_CORE_TRANSPORT_TLSPEERIDENTITY_H_
