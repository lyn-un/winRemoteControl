#include "core/protocol/accessmessage.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace
{
	constexpr char kType[] = "type";
	constexpr char kVersion[] = "version";
	constexpr char kRequestId[] = "requestId";
	constexpr char kDeviceName[] = "deviceName";
	constexpr char kTimeoutSeconds[] = "timeoutSeconds";
	constexpr char kReason[] = "reason";
	constexpr char kAccessRequest[] = "accessRequest";
	constexpr char kAccessPending[] = "accessPending";
	constexpr char kAccessAccepted[] = "accessAccepted";
	constexpr char kAccessRejected[] = "accessRejected";

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
		return InvalidAccessMessageType;
	}
}

QString KAccessMessageCodec::encode(const KAccessMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kType), typeName(message.type));
	object.insert(QString::fromLatin1(kVersion), kProtocolVersion);
	object.insert(QString::fromLatin1(kRequestId), message.strRequestId);
	if (message.type == RequestAccessMessageType)
		object.insert(QString::fromLatin1(kDeviceName), message.strDeviceName.left(128));
	else if (message.type == PendingAccessMessageType)
		object.insert(QString::fromLatin1(kTimeoutSeconds), message.nTimeoutSeconds);
	else if (message.type == RejectedAccessMessageType)
		object.insert(QString::fromLatin1(kReason), message.strReason);
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KAccessMessageCodec::decode(const QString &strMessage,
	KAccessMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return FailDecode(QStringLiteral("Access message output is null"), pErrorMessage);
	if (strMessage.toUtf8().size() > kMaximumMessageBytes)
		return FailDecode(QStringLiteral("Access message is too large"), pErrorMessage);

	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	if (!document.isObject())
		return FailDecode(QStringLiteral("Access message is not a JSON object"), pErrorMessage);
	const QJsonObject object = document.object();
	const KAccessMessageType type = MessageTypeFromName(
		object.value(QString::fromLatin1(kType)).toString());
	if (type == InvalidAccessMessageType)
		return FailDecode(QStringLiteral("Unknown access message type"), pErrorMessage);
	if (object.value(QString::fromLatin1(kVersion)).toInt(-1) != kProtocolVersion)
		return FailDecode(QStringLiteral("Unsupported access protocol version"), pErrorMessage);

	const QString strRequestId = object.value(QString::fromLatin1(kRequestId)).toString();
	if (QUuid(strRequestId).isNull())
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
	if (strMessage.toUtf8().size() > kMaximumMessageBytes)
		return false;
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8());
	return document.isObject()
		&& MessageTypeFromName(document.object().value(QString::fromLatin1(kType)).toString())
			!= InvalidAccessMessageType;
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
