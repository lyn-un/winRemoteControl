#ifndef _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICE_H_
#define _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICE_H_

#include "core/security/permissionscope.h"

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVector>

struct KTrustedDevice
{
	QString strDeviceId;
	QByteArray spkiSha256;
	QByteArray certificateSha256;
	QString strFingerprint;
	QString strAlias;
	QString strAdvertisedName;
	KPermissionScopes permissionLimit;
	qint64 nPairedAtMs = 0;
	qint64 nLastAuthenticatedAtMs = 0;
	bool bRevoked = false;
};

Q_DECLARE_METATYPE(KTrustedDevice)
Q_DECLARE_METATYPE(QVector<KTrustedDevice>)

#endif // _WINREMOTECONTROL_CORE_SECURITY_TRUSTEDDEVICE_H_
