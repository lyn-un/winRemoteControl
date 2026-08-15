#include "core/security/deviceidentity.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QStringList>
#include <QtCore/QUuid>

bool KDeviceIdentity::isValid() const
{
	return !QUuid::fromString(strDeviceId).isNull()
		&& strAlgorithm == QStringLiteral("ecdsa-p256-sha256")
		&& publicKey.size() == 65
		&& publicKey.at(0) == '\x04'
		&& strFingerprint == DevicePublicKeyFingerprint(publicKey);
}

QString DevicePublicKeyFingerprint(const QByteArray &publicKey)
{
	const QByteArray digest = QCryptographicHash::hash(
		publicKey, QCryptographicHash::Sha256).toHex().toUpper();
	QStringList groups;
	for (int nOffset = 0; nOffset < digest.size(); nOffset += 4)
		groups.append(QString::fromLatin1(digest.mid(nOffset, 4)));
	return groups.join(QLatin1Char(':'));
}
