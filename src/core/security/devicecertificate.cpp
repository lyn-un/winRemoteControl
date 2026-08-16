#include "core/security/devicecertificate.h"

#include <QtCore/QUuid>

bool KDeviceCertificate::isValid() const
{
	return !QUuid(strDeviceId).isNull()
		&& !certificateDer.isEmpty()
		&& spkiSha256.size() == 32
		&& certificateSha256.size() == 32
		&& validFromUtc.isValid()
		&& validToUtc.isValid()
		&& validFromUtc < validToUtc;
}

QString KDeviceCertificate::spkiFingerprint() const
{
	if (spkiSha256.size() != 32)
		return QString();
	return QStringLiteral("SHA256:%1").arg(QString::fromLatin1(
		spkiSha256.toBase64(QByteArray::OmitTrailingEquals)));
}
