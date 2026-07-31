#include "core/protocol/sessionmessage.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

namespace
{
	constexpr char kType[] = "type";
	constexpr char kDeviceInfoRequest[] = "deviceInfoRequest";
	constexpr char kDeviceInfo[] = "deviceInfo";
	constexpr char kStartStreaming[] = "startStreaming";
	constexpr char kStopStreaming[] = "stopStreaming";
	constexpr char kEndSession[] = "endSession";
	constexpr char kReason[] = "reason";
	constexpr char kStreamConfig[] = "streamConfig";
	constexpr char kComputerName[] = "computerName";
	constexpr char kWallpaperMime[] = "wallpaperMime";
	constexpr char kWallpaperData[] = "wallpaperData";
	constexpr char kScreenWidth[] = "screenWidth";
	constexpr char kScreenHeight[] = "screenHeight";
	constexpr char kFps[] = "fps";
	constexpr char kWidth[] = "width";
	constexpr char kHeight[] = "height";
	constexpr char kBitrateKbps[] = "bitrateKbps";

	bool readRequiredInt(const QJsonObject &object, const char *pName, int *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isDouble())
			return false;

		const int nValue = value.toInt();
		if (static_cast<double>(nValue) != value.toDouble())
			return false;

		*pValue = nValue;
		return true;
	}

	bool readOptionalInt(const QJsonObject &object, const char *pName, int *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (value.isUndefined())
			return true;
		return readRequiredInt(object, pName, pValue);
	}

	bool failDecode(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}
}

QString KSessionMessageCodec::encode(const KSessionMessage &message)
{
	QJsonObject object;
	object.insert(QString::fromLatin1(kType), typeName(message.type));
	if (message.type == DeviceInfoSessionMessageType)
	{
		object.insert(QString::fromLatin1(kComputerName), message.deviceInfo.strComputerName);
		object.insert(QString::fromLatin1(kScreenWidth), message.deviceInfo.nScreenWidth);
		object.insert(QString::fromLatin1(kScreenHeight), message.deviceInfo.nScreenHeight);
		if (!message.deviceInfo.strWallpaperData.isEmpty())
		{
			object.insert(QString::fromLatin1(kWallpaperMime), message.deviceInfo.strWallpaperMime);
			object.insert(QString::fromLatin1(kWallpaperData), message.deviceInfo.strWallpaperData);
		}
	}
	else if (message.type == EndSessionMessageType)
	{
		object.insert(QString::fromLatin1(kReason), message.strReason);
	}
	else if (message.type == StreamConfigSessionMessageType)
	{
		object.insert(QString::fromLatin1(kFps), message.streamConfig.nFps);
		object.insert(QString::fromLatin1(kWidth), message.streamConfig.nWidth);
		object.insert(QString::fromLatin1(kHeight), message.streamConfig.nHeight);
		object.insert(QString::fromLatin1(kBitrateKbps), message.streamConfig.nBitrateKbps);
	}

	return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool KSessionMessageCodec::decode(const QString &strMessage,
	KSessionMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return failDecode(QStringLiteral("Session message output is null"), pErrorMessage);

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(strMessage.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return failDecode(QStringLiteral("Session message is not a JSON object"), pErrorMessage);

	const QJsonObject object = document.object();
	const QString strType = object.value(QString::fromLatin1(kType)).toString();
	KSessionMessage message;
	if (strType == QString::fromLatin1(kDeviceInfoRequest))
		message.type = DeviceInfoRequestSessionMessageType;
	else if (strType == QString::fromLatin1(kDeviceInfo))
		message.type = DeviceInfoSessionMessageType;
	else if (strType == QString::fromLatin1(kStartStreaming))
		message.type = StartStreamingSessionMessageType;
	else if (strType == QString::fromLatin1(kStopStreaming))
		message.type = StopStreamingSessionMessageType;
	else if (strType == QString::fromLatin1(kEndSession))
		message.type = EndSessionMessageType;
	else if (strType == QString::fromLatin1(kStreamConfig))
		message.type = StreamConfigSessionMessageType;
	else
		return failDecode(QStringLiteral("Unknown session message type"), pErrorMessage);

	if (message.type == DeviceInfoSessionMessageType)
	{
		const QJsonValue computerNameValue = object.value(QString::fromLatin1(kComputerName));
		const QJsonValue wallpaperMimeValue = object.value(QString::fromLatin1(kWallpaperMime));
		const QJsonValue wallpaperDataValue = object.value(QString::fromLatin1(kWallpaperData));
		if (!computerNameValue.isString()
			|| !readRequiredInt(object, kScreenWidth, &message.deviceInfo.nScreenWidth)
			|| !readRequiredInt(object, kScreenHeight, &message.deviceInfo.nScreenHeight)
			|| message.deviceInfo.nScreenWidth < 0
			|| message.deviceInfo.nScreenHeight < 0
			|| (!wallpaperMimeValue.isUndefined() && !wallpaperMimeValue.isString())
			|| (!wallpaperDataValue.isUndefined() && !wallpaperDataValue.isString()))
		{
			return failDecode(QStringLiteral("Invalid device info message"), pErrorMessage);
		}

		message.deviceInfo.strComputerName = computerNameValue.toString();
		message.deviceInfo.strWallpaperMime = wallpaperMimeValue.toString();
		message.deviceInfo.strWallpaperData = wallpaperDataValue.toString();
	}
	else if (message.type == EndSessionMessageType)
	{
		const QJsonValue reasonValue = object.value(QString::fromLatin1(kReason));
		if (!reasonValue.isUndefined() && !reasonValue.isString())
			return failDecode(QStringLiteral("Invalid end session message"), pErrorMessage);
		message.strReason = reasonValue.toString();
	}
	else if (message.type == StreamConfigSessionMessageType)
	{
		if (!readOptionalInt(object, kFps, &message.streamConfig.nFps)
			|| !readOptionalInt(object, kWidth, &message.streamConfig.nWidth)
			|| !readOptionalInt(object, kHeight, &message.streamConfig.nHeight)
			|| !readOptionalInt(object, kBitrateKbps, &message.streamConfig.nBitrateKbps))
		{
			return failDecode(QStringLiteral("Invalid stream config message"), pErrorMessage);
		}
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KSessionMessageCodec::typeName(KSessionMessageType type)
{
	if (type == DeviceInfoRequestSessionMessageType)
		return QString::fromLatin1(kDeviceInfoRequest);
	if (type == DeviceInfoSessionMessageType)
		return QString::fromLatin1(kDeviceInfo);
	if (type == StartStreamingSessionMessageType)
		return QString::fromLatin1(kStartStreaming);
	if (type == StopStreamingSessionMessageType)
		return QString::fromLatin1(kStopStreaming);
	if (type == EndSessionMessageType)
		return QString::fromLatin1(kEndSession);
	if (type == StreamConfigSessionMessageType)
		return QString::fromLatin1(kStreamConfig);
	return QStringLiteral("invalid");
}
