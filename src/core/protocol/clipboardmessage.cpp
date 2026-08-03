#include "core/protocol/clipboardmessage.h"

#include "core/protocol/protocolconstraints.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUuid>

namespace
{
	constexpr char kType[] = "type";
	constexpr char kVersion[] = "version";
	constexpr char kClipboardText[] = "clipboardText";
	constexpr char kClipboardReady[] = "clipboardReady";
	constexpr char kClipboardSyncState[] = "clipboardSyncState";
	constexpr char kMessageId[] = "messageId";
	constexpr char kText[] = "text";
	constexpr char kEnabled[] = "enabled";

	bool FailDecode(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	bool IsValidUuid(const QString &strValue)
	{
		return !strValue.isEmpty() && !QUuid::fromString(strValue).isNull();
	}

	bool HasSupportedVersion(const QJsonObject &object)
	{
		const QJsonValue value = object.value(QString::fromLatin1(kVersion));
		if (value.isUndefined())
			return true;
		return value.isDouble()
			&& value.toDouble() == static_cast<double>(value.toInt())
			&& value.toInt() == KClipboardMessageCodec::kProtocolVersion;
	}
}

QString KClipboardMessageCodec::encode(const KClipboardMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kVersion), kProtocolVersion);
	QString strType = QString::fromLatin1(kClipboardText);
	if (message.type == ReadyClipboardMessageType)
		strType = QString::fromLatin1(kClipboardReady);
	else if (message.type == SyncStateClipboardMessageType)
		strType = QString::fromLatin1(kClipboardSyncState);
	object.insert(QString::fromLatin1(kType), strType);
	object.insert(QString::fromLatin1(kMessageId), message.strMessageId);
	if (message.type == SyncStateClipboardMessageType)
		object.insert(QString::fromLatin1(kEnabled), message.bEnabled);
	else if (message.type == TextClipboardMessageType)
		object.insert(QString::fromLatin1(kText), message.strText);
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KClipboardMessageCodec::decode(const QString &strMessage,
	KClipboardMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return FailDecode(QStringLiteral("Clipboard message output is null"), pErrorMessage);
	const QByteArray data = strMessage.toUtf8();
	if (data.size() > KProtocolConstraints::kMaximumClipboardMessageBytes)
		return FailDecode(QStringLiteral("Clipboard message is too large"), pErrorMessage);

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return FailDecode(QStringLiteral("Clipboard message is not a JSON object"), pErrorMessage);

	const QJsonObject object = document.object();
	if (!HasSupportedVersion(object))
		return FailDecode(QStringLiteral("Unsupported clipboard protocol version"), pErrorMessage);
	const QJsonValue typeValue = object.value(QString::fromLatin1(kType));
	const QJsonValue messageIdValue = object.value(QString::fromLatin1(kMessageId));
	const QString strType = typeValue.toString();
	if (!typeValue.isString()
		|| (strType != QString::fromLatin1(kClipboardText)
			&& strType != QString::fromLatin1(kClipboardReady)
			&& strType != QString::fromLatin1(kClipboardSyncState))
		|| !messageIdValue.isString())
	{
		return FailDecode(QStringLiteral("Invalid clipboard message fields"), pErrorMessage);
	}

	KClipboardMessage message;
	message.strMessageId = messageIdValue.toString();
	if (!IsValidUuid(message.strMessageId))
		return FailDecode(QStringLiteral("Invalid clipboard message UUID"), pErrorMessage);
	if (strType == QString::fromLatin1(kClipboardReady))
	{
		message.type = ReadyClipboardMessageType;
	}
	else if (strType == QString::fromLatin1(kClipboardText))
	{
		const QJsonValue textValue = object.value(QString::fromLatin1(kText));
		if (!textValue.isString())
			return FailDecode(QStringLiteral("Invalid clipboard text field"), pErrorMessage);
		message.type = TextClipboardMessageType;
		message.strText = textValue.toString();
		if (message.strText.toUtf8().size() > kMaximumTextBytes)
			return FailDecode(QStringLiteral("Clipboard text exceeds size limit"), pErrorMessage);
	}
	else
	{
		const QJsonValue enabledValue = object.value(QString::fromLatin1(kEnabled));
		if (!enabledValue.isBool())
			return FailDecode(QStringLiteral("Invalid clipboard sync state field"), pErrorMessage);
		message.type = SyncStateClipboardMessageType;
		message.bEnabled = enabledValue.toBool();
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}
