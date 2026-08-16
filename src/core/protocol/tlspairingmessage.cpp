#include "core/protocol/tlspairingmessage.h"

#include "core/protocol/protocolenvelope.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace
{
	constexpr char kPairingHello[] = "tlsPairingHello";
	constexpr char kPairingDecision[] = "tlsPairingDecision";
	constexpr char kPairingReady[] = "tlsPairingReady";
	constexpr char kPairingRejected[] = "tlsPairingRejected";

	bool Fail(const QString &strMessage, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strMessage;
		return false;
	}

	KTlsPairingMessageType TypeFromName(const QString &strType)
	{
		if (strType == QString::fromLatin1(kPairingHello))
			return HelloTlsPairingMessageType;
		if (strType == QString::fromLatin1(kPairingDecision))
			return DecisionTlsPairingMessageType;
		if (strType == QString::fromLatin1(kPairingReady))
			return ReadyTlsPairingMessageType;
		if (strType == QString::fromLatin1(kPairingRejected))
			return RejectedTlsPairingMessageType;
		return InvalidTlsPairingMessageType;
	}
}

QString KTlsPairingMessageCodec::encode(const KTlsPairingMessage &message)
{
	QJsonObject object;
	if (!message.strDeviceId.isEmpty())
		object.insert(QStringLiteral("deviceId"), message.strDeviceId);
	if (!message.strDeviceName.isEmpty())
		object.insert(QStringLiteral("deviceName"), message.strDeviceName.left(128));
	if (!message.strVerificationMethod.isEmpty())
	{
		object.insert(QStringLiteral("verificationMethod"),
			message.strVerificationMethod.left(64));
	}
	if (message.permissions.toInt() != 0)
	{
		QJsonArray permissions;
		for (const QString &strName : PermissionScopeNames(message.permissions))
			permissions.append(strName);
		object.insert(QStringLiteral("permissions"), permissions);
	}
	if (message.type == DecisionTlsPairingMessageType)
		object.insert(QStringLiteral("accepted"), message.bAccepted);
	if (message.type == RejectedTlsPairingMessageType)
		object.insert(QStringLiteral("reason"), message.strReason);
	return KProtocolEnvelopeCodec::encode(SignalingProtocolChannel,
		typeName(message.type), message.strRequestId, 0, object);
}

bool KTlsPairingMessageCodec::decode(const QString &strMessage,
	KTlsPairingMessage *pMessage,
	QString *pErrorMessage)
{
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	if (envelope.nEncodedBytes > kMaximumMessageBytes)
		return Fail(QStringLiteral("TLS pairing message is too large"), pErrorMessage);
	return decode(envelope, pMessage, pErrorMessage);
}

bool KTlsPairingMessageCodec::decode(const KProtocolEnvelope &envelope,
	KTlsPairingMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("TLS pairing output is null"), pErrorMessage);
	KTlsPairingMessage message;
	message.type = TypeFromName(envelope.strType);
	message.strRequestId = envelope.strRequestId;
	if (message.type == InvalidTlsPairingMessageType
		|| QUuid(message.strRequestId).isNull())
	{
		return Fail(QStringLiteral("Invalid TLS pairing type or request id"), pErrorMessage);
	}
	const QJsonObject object = envelope.payload;
	message.strDeviceId = object.value(QStringLiteral("deviceId")).toString();
	message.strDeviceName = object.value(QStringLiteral("deviceName")).toString();
	message.strVerificationMethod = object.value(
		QStringLiteral("verificationMethod")).toString();
	const QJsonValue permissionsValue = object.value(QStringLiteral("permissions"));
	if (!permissionsValue.isUndefined())
	{
		if (!permissionsValue.isArray())
			return Fail(QStringLiteral("TLS pairing permissions must be an array"), pErrorMessage);
		QStringList names;
		for (const QJsonValue &value : permissionsValue.toArray())
		{
			if (!value.isString())
				return Fail(QStringLiteral("TLS pairing permission is invalid"), pErrorMessage);
			names.append(value.toString());
		}
		if (!PermissionScopesFromNames(names, &message.permissions))
			return Fail(QStringLiteral("TLS pairing permission is unknown"), pErrorMessage);
	}
	if (message.type == HelloTlsPairingMessageType)
	{
		if (QUuid(message.strDeviceId).isNull()
			|| message.strDeviceName.trimmed().isEmpty()
			|| message.strDeviceName.size() > 128
			|| message.strVerificationMethod.isEmpty()
			|| message.strVerificationMethod.size() > 64
			|| !message.permissions.testFlag(ViewScreenPermissionScope))
		{
			return Fail(QStringLiteral("TLS pairing peer fields are invalid"), pErrorMessage);
		}
	}
	else if (message.type == DecisionTlsPairingMessageType)
	{
		const QJsonValue acceptedValue = object.value(QStringLiteral("accepted"));
		if (!acceptedValue.isBool() || QUuid(message.strDeviceId).isNull())
			return Fail(QStringLiteral("TLS pairing decision is invalid"), pErrorMessage);
		message.bAccepted = acceptedValue.toBool();
	}
	else if (message.type == ReadyTlsPairingMessageType)
	{
		if (QUuid(message.strDeviceId).isNull()
			|| !message.permissions.testFlag(ViewScreenPermissionScope))
		{
			return Fail(QStringLiteral("TLS pairing ready message is invalid"), pErrorMessage);
		}
	}
	else
	{
		message.strReason = object.value(QStringLiteral("reason")).toString();
		if (!isValidRejectReason(message.strReason))
			return Fail(QStringLiteral("TLS pairing rejection reason is invalid"), pErrorMessage);
	}
	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KTlsPairingMessageCodec::typeName(KTlsPairingMessageType type)
{
	if (type == HelloTlsPairingMessageType)
		return QString::fromLatin1(kPairingHello);
	if (type == DecisionTlsPairingMessageType)
		return QString::fromLatin1(kPairingDecision);
	if (type == ReadyTlsPairingMessageType)
		return QString::fromLatin1(kPairingReady);
	if (type == RejectedTlsPairingMessageType)
		return QString::fromLatin1(kPairingRejected);
	return QStringLiteral("invalid");
}

bool KTlsPairingMessageCodec::isTlsPairingType(const QString &strType)
{
	return TypeFromName(strType) != InvalidTlsPairingMessageType;
}

bool KTlsPairingMessageCodec::isValidRejectReason(const QString &strReason)
{
	return strReason == QStringLiteral("authentication_timeout")
		|| strReason == QStringLiteral("pairing_rejected")
		|| strReason == QStringLiteral("pairing_rate_limited")
		|| strReason == QStringLiteral("device_key_changed")
		|| strReason == QStringLiteral("device_revoked")
		|| strReason == QStringLiteral("channel_binding_unavailable")
		|| strReason == QStringLiteral("protocol_incompatible")
		|| strReason == QStringLiteral("cancelled");
}
