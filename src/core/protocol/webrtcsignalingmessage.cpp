#include "core/protocol/webrtcsignalingmessage.h"

#include "core/protocol/protocolconstraints.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

namespace
{
	constexpr char kVersion[] = "version";
	constexpr char kMessageType[] = "messageType";
	constexpr char kSdpType[] = "sdpType";
	constexpr char kSdp[] = "sdp";
	constexpr char kSdpMid[] = "sdpMid";
	constexpr char kSdpMLineIndex[] = "sdpMLineIndex";
	constexpr char kCandidate[] = "candidate";
	constexpr char kOffer[] = "offer";
	constexpr char kAnswer[] = "answer";
	constexpr char kIceCandidate[] = "iceCandidate";
	constexpr int kMaximumSdpCharacters = 128 * 1024;
	constexpr int kMaximumCandidateCharacters = 16 * 1024;
	constexpr int kMaximumSdpMidCharacters = 256;
	constexpr int kMaximumSdpMLineIndex = 1024;

	bool FailDecode(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	bool HasSupportedVersion(const QJsonObject &object)
	{
		const QJsonValue value = object.value(QString::fromLatin1(kVersion));
		if (value.isUndefined())
			return true;
		return value.isDouble()
			&& value.toDouble() == static_cast<double>(value.toInt())
			&& value.toInt() == KProtocolConstraints::kProtocolVersion;
	}
}

QString KWebRtcSignalingMessageCodec::encode(const KWebRtcSignalingMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kVersion), KProtocolConstraints::kProtocolVersion);
	object.insert(QString::fromLatin1(kMessageType), typeName(message.type));
	if (message.type == OfferWebRtcSignalingMessageType
		|| message.type == AnswerWebRtcSignalingMessageType)
	{
		object.insert(QString::fromLatin1(kSdpType), typeName(message.type));
		object.insert(QString::fromLatin1(kSdp), message.strSdp);
	}
	else if (message.type == IceCandidateWebRtcSignalingMessageType)
	{
		object.insert(QString::fromLatin1(kSdpMid), message.strSdpMid);
		object.insert(QString::fromLatin1(kSdpMLineIndex), message.nSdpMLineIndex);
		object.insert(QString::fromLatin1(kCandidate), message.strCandidate);
	}
	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KWebRtcSignalingMessageCodec::decode(const QString &strMessage,
	KWebRtcSignalingMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return FailDecode(QStringLiteral("Signaling message output is null"), pErrorMessage);
	const QByteArray data = strMessage.toUtf8();
	if (data.size() > KProtocolConstraints::kMaximumSignalingMessageBytes)
		return FailDecode(QStringLiteral("Signaling message is too large"), pErrorMessage);

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return FailDecode(QStringLiteral("Signaling message is not a JSON object"), pErrorMessage);
	const QJsonObject object = document.object();
	if (!HasSupportedVersion(object))
		return FailDecode(QStringLiteral("Unsupported signaling protocol version"), pErrorMessage);

	const QString strType = object.value(QString::fromLatin1(kMessageType)).toString();
	KWebRtcSignalingMessage message;
	if (strType == QString::fromLatin1(kOffer))
		message.type = OfferWebRtcSignalingMessageType;
	else if (strType == QString::fromLatin1(kAnswer))
		message.type = AnswerWebRtcSignalingMessageType;
	else if (strType == QString::fromLatin1(kIceCandidate))
		message.type = IceCandidateWebRtcSignalingMessageType;
	else
		return FailDecode(QStringLiteral("Unknown signaling message type"), pErrorMessage);

	if (message.type == OfferWebRtcSignalingMessageType
		|| message.type == AnswerWebRtcSignalingMessageType)
	{
		const QJsonValue sdpTypeValue = object.value(QString::fromLatin1(kSdpType));
		const QJsonValue sdpValue = object.value(QString::fromLatin1(kSdp));
		if (!sdpTypeValue.isString()
			|| sdpTypeValue.toString() != strType
			|| !sdpValue.isString()
			|| sdpValue.toString().isEmpty()
			|| sdpValue.toString().size() > kMaximumSdpCharacters)
		{
			return FailDecode(QStringLiteral("Invalid signaling session description"), pErrorMessage);
		}
		message.strSdp = sdpValue.toString();
	}
	else
	{
		const QJsonValue sdpMidValue = object.value(QString::fromLatin1(kSdpMid));
		const QJsonValue lineIndexValue = object.value(QString::fromLatin1(kSdpMLineIndex));
		const QJsonValue candidateValue = object.value(QString::fromLatin1(kCandidate));
		if (!sdpMidValue.isString()
			|| sdpMidValue.toString().size() > kMaximumSdpMidCharacters
			|| !lineIndexValue.isDouble()
			|| lineIndexValue.toDouble() != static_cast<double>(lineIndexValue.toInt())
			|| lineIndexValue.toInt() < 0
			|| lineIndexValue.toInt() > kMaximumSdpMLineIndex
			|| !candidateValue.isString()
			|| candidateValue.toString().isEmpty()
			|| candidateValue.toString().size() > kMaximumCandidateCharacters)
		{
			return FailDecode(QStringLiteral("Invalid signaling ICE candidate"), pErrorMessage);
		}
		message.strSdpMid = sdpMidValue.toString();
		message.nSdpMLineIndex = lineIndexValue.toInt();
		message.strCandidate = candidateValue.toString();
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KWebRtcSignalingMessageCodec::typeName(KWebRtcSignalingMessageType type)
{
	if (type == OfferWebRtcSignalingMessageType)
		return QString::fromLatin1(kOffer);
	if (type == AnswerWebRtcSignalingMessageType)
		return QString::fromLatin1(kAnswer);
	if (type == IceCandidateWebRtcSignalingMessageType)
		return QString::fromLatin1(kIceCandidate);
	return QStringLiteral("invalid");
}
