#include "core/protocol/identitymessage.h"

#include "core/protocol/protocolenvelope.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace
{
	constexpr char kIdentityHello[] = "identityHello";
	constexpr char kIdentityChallenge[] = "identityChallenge";
	constexpr char kIdentityProof[] = "identityProof";
	constexpr char kPairingDecision[] = "pairingDecision";
	constexpr char kIdentityAuthenticated[] = "identityAuthenticated";
	constexpr char kIdentityRejected[] = "identityRejected";

	QString EncodeBytes(const QByteArray &value)
	{
		return QString::fromLatin1(value.toBase64(
			QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	}

	QByteArray DecodeBytes(const QJsonValue &value)
	{
		if (!value.isString())
			return QByteArray();
		return QByteArray::fromBase64(value.toString().toLatin1(),
			QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
	}

	bool Fail(const QString &strMessage, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strMessage;
		return false;
	}

	KIdentityMessageType TypeFromName(const QString &strType)
	{
		if (strType == QString::fromLatin1(kIdentityHello))
			return HelloIdentityMessageType;
		if (strType == QString::fromLatin1(kIdentityChallenge))
			return ChallengeIdentityMessageType;
		if (strType == QString::fromLatin1(kIdentityProof))
			return ProofIdentityMessageType;
		if (strType == QString::fromLatin1(kPairingDecision))
			return PairingDecisionIdentityMessageType;
		if (strType == QString::fromLatin1(kIdentityAuthenticated))
			return AuthenticatedIdentityMessageType;
		if (strType == QString::fromLatin1(kIdentityRejected))
			return RejectedIdentityMessageType;
		return InvalidIdentityMessageType;
	}
}

QString KIdentityMessageCodec::encode(const KIdentityMessage &message)
{
	QJsonObject object;
	if (!message.strDeviceId.isEmpty())
		object.insert(QStringLiteral("deviceId"), message.strDeviceId);
	if (!message.strDeviceName.isEmpty())
		object.insert(QStringLiteral("deviceName"), message.strDeviceName.left(128));
	if (!message.publicKey.isEmpty())
		object.insert(QStringLiteral("publicKey"), EncodeBytes(message.publicKey));
	if (!message.nonce.isEmpty())
		object.insert(QStringLiteral("nonce"), EncodeBytes(message.nonce));
	if (!message.signature.isEmpty())
		object.insert(QStringLiteral("signature"), EncodeBytes(message.signature));
	if (!message.transcriptHash.isEmpty())
		object.insert(QStringLiteral("transcriptHash"), EncodeBytes(message.transcriptHash));
	if (message.permissions.toInt() != 0)
	{
		QJsonArray permissions;
		for (const QString &strName : PermissionScopeNames(message.permissions))
			permissions.append(strName);
		object.insert(QStringLiteral("permissions"), permissions);
	}
	if (message.type == PairingDecisionIdentityMessageType)
		object.insert(QStringLiteral("accepted"), message.bAccepted);
	if (message.type == RejectedIdentityMessageType)
		object.insert(QStringLiteral("reason"), message.strReason);
	return KProtocolEnvelopeCodec::encode(SignalingProtocolChannel,
		typeName(message.type), message.strRequestId, 0, object);
}

bool KIdentityMessageCodec::decode(const QString &strMessage,
	KIdentityMessage *pMessage,
	QString *pErrorMessage)
{
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	if (envelope.nEncodedBytes > kMaximumMessageBytes)
		return Fail(QStringLiteral("Identity message is too large"), pErrorMessage);
	return decode(envelope, pMessage, pErrorMessage);
}

bool KIdentityMessageCodec::decode(const KProtocolEnvelope &envelope,
	KIdentityMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("Identity message output is null"), pErrorMessage);
	KIdentityMessage message;
	message.type = TypeFromName(envelope.strType);
	message.strRequestId = envelope.strRequestId;
	if (message.type == InvalidIdentityMessageType
		|| QUuid(message.strRequestId).isNull())
	{
		return Fail(QStringLiteral("Invalid identity message type or request id"), pErrorMessage);
	}
	const QJsonObject object = envelope.payload;
	message.strDeviceId = object.value(QStringLiteral("deviceId")).toString();
	message.strDeviceName = object.value(QStringLiteral("deviceName")).toString();
	message.publicKey = DecodeBytes(object.value(QStringLiteral("publicKey")));
	message.nonce = DecodeBytes(object.value(QStringLiteral("nonce")));
	message.signature = DecodeBytes(object.value(QStringLiteral("signature")));
	message.transcriptHash = DecodeBytes(object.value(QStringLiteral("transcriptHash")));
	const QJsonValue permissionsValue = object.value(QStringLiteral("permissions"));
	if (!permissionsValue.isUndefined())
	{
		if (!permissionsValue.isArray())
			return Fail(QStringLiteral("Identity permissions must be an array"), pErrorMessage);
		QStringList names;
		for (const QJsonValue &value : permissionsValue.toArray())
		{
			if (!value.isString())
				return Fail(QStringLiteral("Identity permission is invalid"), pErrorMessage);
			names.append(value.toString());
		}
		if (!PermissionScopesFromNames(names, &message.permissions))
			return Fail(QStringLiteral("Identity permission is unknown"), pErrorMessage);
	}
	if (message.type == HelloIdentityMessageType
		|| message.type == ChallengeIdentityMessageType)
	{
		if (QUuid(message.strDeviceId).isNull()
			|| message.strDeviceName.trimmed().isEmpty()
			|| message.strDeviceName.size() > 128
			|| message.publicKey.size() != 65 || message.publicKey.at(0) != '\x04'
			|| message.nonce.size() != 32)
		{
			return Fail(QStringLiteral("Identity peer fields are invalid"), pErrorMessage);
		}
		if (message.type == HelloIdentityMessageType
			&& !message.permissions.testFlag(ViewScreenPermissionScope))
		{
			return Fail(QStringLiteral("View permission is required"), pErrorMessage);
		}
		if (message.type == ChallengeIdentityMessageType
			&& message.signature.size() != 64)
		{
			return Fail(QStringLiteral("Identity challenge signature is invalid"), pErrorMessage);
		}
	}
	else if (message.type == ProofIdentityMessageType
		|| message.type == AuthenticatedIdentityMessageType)
	{
		if (message.strDeviceId.isEmpty() || message.signature.size() != 64
			|| message.transcriptHash.size() != 32)
		{
			return Fail(QStringLiteral("Identity proof fields are invalid"), pErrorMessage);
		}
	}
	else if (message.type == PairingDecisionIdentityMessageType)
	{
		const QJsonValue acceptedValue = object.value(QStringLiteral("accepted"));
		if (!acceptedValue.isBool() || message.strDeviceId.isEmpty()
			|| message.signature.size() != 64 || message.transcriptHash.size() != 32)
		{
			return Fail(QStringLiteral("Pairing decision fields are invalid"), pErrorMessage);
		}
		message.bAccepted = acceptedValue.toBool();
	}
	else if (message.type == RejectedIdentityMessageType)
	{
		message.strReason = object.value(QStringLiteral("reason")).toString();
		if (!isValidRejectReason(message.strReason))
			return Fail(QStringLiteral("Identity rejection reason is invalid"), pErrorMessage);
	}
	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KIdentityMessageCodec::typeName(KIdentityMessageType type)
{
	if (type == HelloIdentityMessageType)
		return QString::fromLatin1(kIdentityHello);
	if (type == ChallengeIdentityMessageType)
		return QString::fromLatin1(kIdentityChallenge);
	if (type == ProofIdentityMessageType)
		return QString::fromLatin1(kIdentityProof);
	if (type == PairingDecisionIdentityMessageType)
		return QString::fromLatin1(kPairingDecision);
	if (type == AuthenticatedIdentityMessageType)
		return QString::fromLatin1(kIdentityAuthenticated);
	if (type == RejectedIdentityMessageType)
		return QString::fromLatin1(kIdentityRejected);
	return QStringLiteral("invalid");
}

bool KIdentityMessageCodec::isIdentityType(const QString &strType)
{
	return TypeFromName(strType) != InvalidIdentityMessageType;
}

bool KIdentityMessageCodec::isValidRejectReason(const QString &strReason)
{
	return strReason == QStringLiteral("authentication_timeout")
		|| strReason == QStringLiteral("signature_invalid")
		|| strReason == QStringLiteral("pairing_rejected")
		|| strReason == QStringLiteral("pairing_rate_limited")
		|| strReason == QStringLiteral("device_key_changed")
		|| strReason == QStringLiteral("device_revoked")
		|| strReason == QStringLiteral("protocol_incompatible")
		|| strReason == QStringLiteral("cancelled");
}
