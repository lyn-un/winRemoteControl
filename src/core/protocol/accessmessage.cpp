#include "core/protocol/accessmessage.h"

#include "core/protocol/protocolenvelope.h"

#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace
{
	constexpr char kDeviceName[] = "deviceName";
	constexpr char kTimeoutSeconds[] = "timeoutSeconds";
	constexpr char kReason[] = "reason";
	constexpr char kAccessRequest[] = "accessRequest";
	constexpr char kAccessPending[] = "accessPending";
	constexpr char kAccessAccepted[] = "accessAccepted";
	constexpr char kAccessRejected[] = "accessRejected";
	constexpr char kServerBusy[] = "serverBusy";

	bool FailDecode(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	KAccessMessageType MessageTypeFromName(const QString &strName)
	{
		if (strName == QString::fromLatin1(kAccessRequest))
			return RequestAccessMessageType;
		if (strName == QString::fromLatin1(kAccessPending))
			return PendingAccessMessageType;
		if (strName == QString::fromLatin1(kAccessAccepted))
			return AcceptedAccessMessageType;
		if (strName == QString::fromLatin1(kAccessRejected))
			return RejectedAccessMessageType;
		if (strName == QString::fromLatin1(kServerBusy))
			return ServerBusyAccessMessageType;
		return InvalidAccessMessageType;
	}
}

QString KAccessMessageCodec::encode(const KAccessMessage &message)
{
	QJsonObject object;
	if (message.type == RequestAccessMessageType)
		object.insert(QString::fromLatin1(kDeviceName), message.strDeviceName.left(128));
	else if (message.type == PendingAccessMessageType)
		object.insert(QString::fromLatin1(kTimeoutSeconds), message.nTimeoutSeconds);
	else if (message.type == RejectedAccessMessageType)
		object.insert(QString::fromLatin1(kReason), message.strReason);
	return KProtocolEnvelopeCodec::encode(SignalingProtocolChannel,
		typeName(message.type), message.strRequestId, 0, object);
}

bool KAccessMessageCodec::decode(const QString &strMessage,
	KAccessMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return FailDecode(QStringLiteral("Access message output is null"), pErrorMessage);
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	if (envelope.nEncodedBytes > kMaximumMessageBytes)
		return FailDecode(QStringLiteral("Access message is too large"), pErrorMessage);
	if (envelope.nVersion != KProtocolConstraints::kEnvelopeSchemaVersion)
		return FailDecode(QStringLiteral("Unsupported access envelope version"), pErrorMessage);
	return decode(envelope, pMessage, pErrorMessage);
}

bool KAccessMessageCodec::decode(const KProtocolEnvelope &envelope,
	KAccessMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return FailDecode(QStringLiteral("Access message output is null"), pErrorMessage);
	const QJsonObject object = envelope.payload;
	const KAccessMessageType type = MessageTypeFromName(envelope.strType);
	if (type == InvalidAccessMessageType)
		return FailDecode(QStringLiteral("Unknown access message type"), pErrorMessage);

	const QString strRequestId = envelope.strRequestId;
	if (type != ServerBusyAccessMessageType && QUuid(strRequestId).isNull())
		return FailDecode(QStringLiteral("Invalid access request id"), pErrorMessage);

	KAccessMessage message;
	message.type = type;
	message.strRequestId = strRequestId;
	if (type == RequestAccessMessageType)
	{
		const QJsonValue deviceNameValue = object.value(QString::fromLatin1(kDeviceName));
		if (!deviceNameValue.isString()
			|| deviceNameValue.toString().trimmed().isEmpty()
			|| deviceNameValue.toString().size() > 128)
		{
			return FailDecode(QStringLiteral("Invalid access device name"), pErrorMessage);
		}
		message.strDeviceName = deviceNameValue.toString().trimmed();
	}
	else if (type == PendingAccessMessageType)
	{
		const QJsonValue timeoutValue = object.value(QString::fromLatin1(kTimeoutSeconds));
		if (!timeoutValue.isDouble()
			|| timeoutValue.toInt() < 10
			|| timeoutValue.toInt() > 120
			|| static_cast<double>(timeoutValue.toInt()) != timeoutValue.toDouble())
		{
			return FailDecode(QStringLiteral("Invalid access approval timeout"), pErrorMessage);
		}
		message.nTimeoutSeconds = timeoutValue.toInt();
	}
	else if (type == RejectedAccessMessageType)
	{
		message.strReason = object.value(QString::fromLatin1(kReason)).toString();
		if (!isValidRejectReason(message.strReason))
			return FailDecode(QStringLiteral("Invalid access rejection reason"), pErrorMessage);
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

bool KAccessMessageCodec::isAccessMessage(const QString &strMessage)
{
	KProtocolEnvelope envelope;
	return KProtocolEnvelopeCodec::decode(SignalingProtocolChannel,
		strMessage, &envelope, nullptr)
		&& MessageTypeFromName(envelope.strType) != InvalidAccessMessageType;
}

QString KAccessMessageCodec::typeName(KAccessMessageType type)
{
	if (type == RequestAccessMessageType)
		return QString::fromLatin1(kAccessRequest);
	if (type == PendingAccessMessageType)
		return QString::fromLatin1(kAccessPending);
	if (type == AcceptedAccessMessageType)
		return QString::fromLatin1(kAccessAccepted);
	if (type == RejectedAccessMessageType)
		return QString::fromLatin1(kAccessRejected);
	if (type == ServerBusyAccessMessageType)
		return QString::fromLatin1(kServerBusy);
	return QStringLiteral("invalid");
}

bool KAccessMessageCodec::isValidRejectReason(const QString &strReason)
{
	return strReason == QStringLiteral("user_rejected")
		|| strReason == QStringLiteral("timeout")
		|| strReason == QStringLiteral("remote_access_disabled")
		|| strReason == QStringLiteral("busy")
		|| strReason == QStringLiteral("invalid_request")
		|| strReason == QStringLiteral("cancelled");
}
