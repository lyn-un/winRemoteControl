#include "core/protocol/protocolenvelope.h"

#include "core/protocol/protocolconstraints.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtCore/QUuid>

#include <cmath>

namespace
{
	constexpr char kVersion[] = "version";
	constexpr char kType[] = "type";
	constexpr char kMessageType[] = "messageType";
	constexpr char kRequestId[] = "requestId";
	constexpr char kSequence[] = "seq";
	constexpr char kPayload[] = "payload";
	constexpr double kMaximumExactJsonInteger = 9007199254740991.0;

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
		if (channel == FileControlProtocolChannel)
			return KProtocolConstraints::kMaximumFileControlMessageBytes;
		return 0;
	}

	bool ReadSequence(const QJsonValue &value, quint64 *pSequence)
	{
		if (value.isUndefined())
		{
			*pSequence = 0;
			return true;
		}
		if (value.isString())
		{
			const QString strSequence = value.toString();
			if (strSequence.isEmpty())
				return false;
			for (const QChar character : strSequence)
			{
				if (!character.isDigit())
					return false;
			}
			bool bOk = false;
			const quint64 nSequence = strSequence.toULongLong(&bOk);
			if (!bOk)
				return false;
			*pSequence = nSequence;
			return true;
		}
		if (!value.isDouble())
			return false;
		const double nSequence = value.toDouble();
		if (nSequence < 0
			|| nSequence > kMaximumExactJsonInteger
			|| std::floor(nSequence) != nSequence)
		{
			return false;
		}
		*pSequence = static_cast<quint64>(nSequence);
		return true;
	}
}

QString KProtocolEnvelopeCodec::encode(KProtocolChannel,
	const QString &strType,
	const QString &strRequestId,
	quint64 nSequence,
	const QJsonObject &payload)
{
	QJsonObject object = payload;
	object.insert(QString::fromLatin1(kVersion), KProtocolConstraints::kEnvelopeSchemaVersion);
	object.insert(QString::fromLatin1(kType), strType);
	if (!strRequestId.isEmpty())
		object.insert(QString::fromLatin1(kRequestId), strRequestId);
	if (nSequence != 0)
		object.insert(QString::fromLatin1(kSequence), QString::number(nSequence));
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
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
	const int nVersion = versionValue.toInt(0);
	QJsonValue typeValue = object.value(QString::fromLatin1(kType));
	if (typeValue.isUndefined())
		typeValue = object.value(QString::fromLatin1(kMessageType));
	if (!versionValue.isDouble()
		|| versionValue.toDouble() != static_cast<double>(nVersion)
		|| nVersion <= 0
		|| !typeValue.isString()
		|| typeValue.toString().isEmpty())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol envelope fields are invalid");
		return false;
	}

	const QJsonValue requestIdValue = object.value(QString::fromLatin1(kRequestId));
	const QString strRequestId = requestIdValue.toString();
	if (!requestIdValue.isUndefined()
		&& (!requestIdValue.isString()
			|| strRequestId.size() > KProtocolConstraints::kMaximumRequestIdCharacters
			|| QUuid(strRequestId).isNull()))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol request id is invalid");
		return false;
	}

	quint64 nSequence = 0;
	const QJsonValue sequenceValue = object.value(QString::fromLatin1(kSequence));
	if (!ReadSequence(sequenceValue, &nSequence))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol sequence is invalid");
		return false;
	}

	pEnvelope->nVersion = nVersion;
	pEnvelope->channel = channel;
	pEnvelope->strType = typeValue.toString();
	pEnvelope->strRequestId = strRequestId;
	pEnvelope->nSequence = nSequence;
	const QJsonValue payloadValue = object.value(QString::fromLatin1(kPayload));
	if (!payloadValue.isUndefined() && !payloadValue.isObject())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Protocol payload is invalid");
		return false;
	}
	if (payloadValue.isObject())
	{
		pEnvelope->payload = payloadValue.toObject();
	}
	else
	{
		pEnvelope->payload = object;
		pEnvelope->payload.remove(QString::fromLatin1(kVersion));
		pEnvelope->payload.remove(QString::fromLatin1(kType));
		pEnvelope->payload.remove(QString::fromLatin1(kMessageType));
		pEnvelope->payload.remove(QString::fromLatin1(kRequestId));
		pEnvelope->payload.remove(QString::fromLatin1(kSequence));
	}
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
	if (channel == FileControlProtocolChannel)
		return QStringLiteral("file-control");
	return QStringLiteral("invalid");
}
