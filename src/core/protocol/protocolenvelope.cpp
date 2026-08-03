#include "core/protocol/protocolenvelope.h"

#include "core/protocol/protocolconstraints.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>

namespace
{
	constexpr char kVersion[] = "version";
	constexpr char kType[] = "type";
	constexpr char kMessageType[] = "messageType";
	constexpr char kRequestId[] = "requestId";
	constexpr char kSequence[] = "seq";
	constexpr char kPayload[] = "payload";

	int MaximumMessageBytes(KProtocolChannel channel)
	{
		if (channel == SignalingProtocolChannel)
			return KProtocolConstraints::kMaximumSignalingMessageBytes;
		if (channel == InputProtocolChannel)
			return KProtocolConstraints::kMaximumInputMessageBytes;
		if (channel == SessionProtocolChannel)
			return KProtocolConstraints::kMaximumSessionMessageBytes;
		if (channel == ClipboardProtocolChannel)
			return KProtocolConstraints::kMaximumClipboardMessageBytes;
		return 0;
	}
}

bool KProtocolEnvelopeCodec::decode(KProtocolChannel channel,
	const QString &strMessage,
	KProtocolEnvelope *pEnvelope,
	QString *pErrorMessage)
{
	if (pEnvelope == nullptr)
		return false;
	*pEnvelope = KProtocolEnvelope();
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();

	const QByteArray data = strMessage.toUtf8();
	const int nMaximumBytes = MaximumMessageBytes(channel);
	if (nMaximumBytes <= 0 || data.size() > nMaximumBytes)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol message exceeds channel limit");
		return false;
	}

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (!document.isObject())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol message must be a JSON object");
		return false;
	}

	const QJsonObject object = document.object();
	const QJsonValue versionValue = object.value(QString::fromLatin1(kVersion));
	const int nVersion = versionValue.isUndefined()
		? KProtocolConstraints::kProtocolVersion
		: versionValue.toInt(0);
	QJsonValue typeValue = object.value(QString::fromLatin1(kType));
	if (typeValue.isUndefined())
		typeValue = object.value(QString::fromLatin1(kMessageType));
	if ((!versionValue.isUndefined()
			&& (!versionValue.isDouble()
				|| versionValue.toDouble() != static_cast<double>(nVersion)))
		|| nVersion <= 0
		|| !typeValue.isString()
		|| typeValue.toString().isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol envelope fields are invalid");
		return false;
	}

	quint64 nSequence = 0;
	const QJsonValue sequenceValue = object.value(QString::fromLatin1(kSequence));
	if (sequenceValue.isString())
		nSequence = sequenceValue.toString().toULongLong();
	else if (sequenceValue.isDouble() && sequenceValue.toDouble() >= 0)
		nSequence = static_cast<quint64>(sequenceValue.toDouble());

	pEnvelope->nVersion = nVersion;
	pEnvelope->channel = channel;
	pEnvelope->strType = typeValue.toString();
	pEnvelope->strRequestId = object.value(QString::fromLatin1(kRequestId)).toString();
	pEnvelope->nSequence = nSequence;
	const QJsonValue payloadValue = object.value(QString::fromLatin1(kPayload));
	pEnvelope->payload = payloadValue.isObject() ? payloadValue.toObject() : object;
	pEnvelope->strRawMessage = strMessage;
	pEnvelope->nEncodedBytes = data.size();
	return true;
}

QString KProtocolEnvelopeCodec::channelName(KProtocolChannel channel)
{
	if (channel == SignalingProtocolChannel)
		return QStringLiteral("signaling");
	if (channel == InputProtocolChannel)
		return QStringLiteral("input");
	if (channel == SessionProtocolChannel)
		return QStringLiteral("session");
	if (channel == ClipboardProtocolChannel)
		return QStringLiteral("clipboard");
	return QStringLiteral("invalid");
}
