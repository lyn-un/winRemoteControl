#include "core/protocol/landiscoverymessage.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QUuid>

namespace
{
	constexpr char kProtocol[] = "wrc-lan-discovery";
	constexpr int kLanDiscoveryProtocolVersion = 1;
	constexpr char kProtocolField[] = "protocol";
	constexpr char kVersionField[] = "version";
	constexpr char kTypeField[] = "type";
	constexpr char kProbeType[] = "probe";
	constexpr char kAnnounceType[] = "announce";
	constexpr char kRequestIdField[] = "requestId";
	constexpr char kInstanceIdField[] = "instanceId";
	constexpr char kDeviceNameField[] = "deviceName";
	constexpr char kSignalingPortField[] = "signalingPort";

	bool failDecode(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	bool isValidUuid(const QJsonValue &value, QString *pUuid)
	{
		if (!value.isString())
			return false;
		const QString strUuid = value.toString();
		if (QUuid(strUuid).isNull())
			return false;
		*pUuid = QUuid(strUuid).toString(QUuid::WithoutBraces);
		return true;
	}

	bool readPort(const QJsonValue &value, quint16 *pPort)
	{
		if (!value.isDouble())
			return false;
		const double dPort = value.toDouble();
		const int nPort = value.toInt();
		if (dPort != static_cast<double>(nPort) || nPort <= 0 || nPort > 65535)
			return false;
		*pPort = static_cast<quint16>(nPort);
		return true;
	}
}

QByteArray KLanDiscoveryMessageCodec::encode(const KLanDiscoveryMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kProtocolField), QString::fromLatin1(kProtocol));
	object.insert(QString::fromLatin1(kVersionField), kLanDiscoveryProtocolVersion);
	object.insert(QString::fromLatin1(kTypeField), typeName(message.type));
	object.insert(QString::fromLatin1(kRequestIdField), message.strRequestId);
	if (message.type == AnnounceLanDiscoveryMessageType)
	{
		object.insert(QString::fromLatin1(kInstanceIdField), message.strInstanceId);
		object.insert(QString::fromLatin1(kDeviceNameField), message.strDeviceName);
		object.insert(QString::fromLatin1(kSignalingPortField), message.nSignalingPort);
	}
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool KLanDiscoveryMessageCodec::decode(const QByteArray &data,
	KLanDiscoveryMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return failDecode(QStringLiteral("Discovery message output is null"), pErrorMessage);
	if (data.isEmpty() || data.size() > kMaximumDatagramSize)
		return failDecode(QStringLiteral("Discovery datagram size is invalid"), pErrorMessage);

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return failDecode(QStringLiteral("Discovery message is not a JSON object"), pErrorMessage);

	const QJsonObject object = document.object();
	if (object.value(QString::fromLatin1(kProtocolField)).toString()
		!= QString::fromLatin1(kProtocol))
	{
		return failDecode(QStringLiteral("Unknown discovery protocol"), pErrorMessage);
	}
	const QJsonValue versionValue = object.value(QString::fromLatin1(kVersionField));
	if (!versionValue.isDouble()
		|| versionValue.toDouble() != static_cast<double>(versionValue.toInt())
		|| versionValue.toInt() != kLanDiscoveryProtocolVersion)
	{
		return failDecode(QStringLiteral("Unsupported discovery protocol version"), pErrorMessage);
	}

	KLanDiscoveryMessage message;
	const QString strType = object.value(QString::fromLatin1(kTypeField)).toString();
	if (strType == QString::fromLatin1(kProbeType))
		message.type = ProbeLanDiscoveryMessageType;
	else if (strType == QString::fromLatin1(kAnnounceType))
		message.type = AnnounceLanDiscoveryMessageType;
	else
		return failDecode(QStringLiteral("Unknown discovery message type"), pErrorMessage);

	if (!isValidUuid(object.value(QString::fromLatin1(kRequestIdField)), &message.strRequestId))
		return failDecode(QStringLiteral("Invalid discovery request ID"), pErrorMessage);

	if (message.type == AnnounceLanDiscoveryMessageType)
	{
		const QJsonValue nameValue = object.value(QString::fromLatin1(kDeviceNameField));
		if (!isValidUuid(object.value(QString::fromLatin1(kInstanceIdField)),
				&message.strInstanceId)
			|| !nameValue.isString()
			|| nameValue.toString().isEmpty()
			|| nameValue.toString().size() > kMaximumDeviceNameLength
			|| !readPort(object.value(QString::fromLatin1(kSignalingPortField)),
				&message.nSignalingPort))
		{
			return failDecode(QStringLiteral("Invalid discovery announcement"), pErrorMessage);
		}
		message.strDeviceName = nameValue.toString();
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KLanDiscoveryMessageCodec::typeName(KLanDiscoveryMessageType type)
{
	if (type == ProbeLanDiscoveryMessageType)
		return QString::fromLatin1(kProbeType);
	if (type == AnnounceLanDiscoveryMessageType)
		return QString::fromLatin1(kAnnounceType);
	return QStringLiteral("invalid");
}
