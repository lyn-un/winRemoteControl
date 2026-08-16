#include "core/security/tlspairingverification.h"

#include "core/security/securitycanonicalwriter.h"

#include <QtCore/QUuid>

namespace
{
	constexpr quint8 kPairingContextVersion = 1;
	constexpr int kSpkiSha256Bytes = 32;

	bool Fail(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	QByteArray UuidBytes(const QString &strUuid)
	{
		const QUuid uuid(strUuid);
		return uuid.isNull() ? QByteArray() : uuid.toRfc4122();
	}
}

QString KTlsPairingVerification::verificationMethod()
{
	return QStringLiteral("tls-exporter-numeric-v1");
}

QByteArray KTlsPairingVerification::exporterLabel()
{
	return QByteArrayLiteral("EXPERIMENTAL-winRemoteControl-pairing-v1");
}

QByteArray KTlsPairingVerification::createContext(
	const QString &strRequestId,
	const QString &strControllerDeviceId,
	const QString &strControlledDeviceId,
	const QByteArray &controllerSpkiSha256,
	const QByteArray &controlledSpkiSha256,
	QString *pErrorMessage)
{
	const QByteArray requestId = UuidBytes(strRequestId);
	const QByteArray controllerDeviceId = UuidBytes(strControllerDeviceId);
	const QByteArray controlledDeviceId = UuidBytes(strControlledDeviceId);
	if (requestId.isEmpty() || controllerDeviceId.isEmpty()
		|| controlledDeviceId.isEmpty())
	{
		Fail(QStringLiteral("Pairing identifiers are invalid"), pErrorMessage);
		return QByteArray();
	}
	if (controllerSpkiSha256.size() != kSpkiSha256Bytes
		|| controlledSpkiSha256.size() != kSpkiSha256Bytes)
	{
		Fail(QStringLiteral("Pairing SPKI fingerprints are invalid"), pErrorMessage);
		return QByteArray();
	}

	KSecurityCanonicalWriter writer;
	writer.appendUInt8(kPairingContextVersion);
	writer.appendBytes(requestId);
	writer.appendBytes(controllerDeviceId);
	writer.appendBytes(controlledDeviceId);
	writer.appendBytes(controllerSpkiSha256);
	writer.appendBytes(controlledSpkiSha256);
	return writer.data();
}

QString KTlsPairingVerification::numericCode(const QByteArray &keyingMaterial,
	QString *pErrorMessage)
{
	if (keyingMaterial.size() < 4)
	{
		Fail(QStringLiteral("TLS keying material is too short"), pErrorMessage);
		return QString();
	}
	quint32 nValue = 0;
	for (int nIndex = 0; nIndex < 4; ++nIndex)
	{
		nValue = (nValue << 8)
			| static_cast<quint8>(keyingMaterial.at(nIndex));
	}
	const quint32 nCode = nValue % 1000000U;
	const QString strDigits = QStringLiteral("%1").arg(nCode, 6, 10, QLatin1Char('0'));
	return strDigits.left(3) + QLatin1Char(' ') + strDigits.mid(3);
}
