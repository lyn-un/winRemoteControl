#include "core/transport/tlspeeridentity.h"

#include <QtCore/QUuid>

bool KTlsPeerIdentity::isValid() const
{
	return !QUuid(strDeviceId).isNull()
		&& spkiSha256.size() == 32
		&& certificateSha256.size() == 32
		&& validFromUtc.isValid()
		&& validToUtc.isValid()
		&& !strTlsProtocol.isEmpty()
		&& !strCipherSuite.isEmpty();
}

QString KTlsPeerIdentity::spkiFingerprint() const
{
	if (spkiSha256.size() != 32)
		return QString();
	return QStringLiteral("SHA256:%1").arg(QString::fromLatin1(
		spkiSha256.toBase64(QByteArray::OmitTrailingEquals)));
}
