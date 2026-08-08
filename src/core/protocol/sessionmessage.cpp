#include "core/protocol/sessionmessage.h"

#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace
{
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
	constexpr char kCapabilities[] = "capabilities";
	constexpr char kCapabilityRejected[] = "capabilityRejected";
	constexpr char kProtocolMinVersion[] = "protocolMinVersion";
	constexpr char kProtocolMaxVersion[] = "protocolMaxVersion";
	constexpr char kSupportedCodecs[] = "supportedCodecs";
	constexpr char kSupportedChannels[] = "supportedChannels";
	constexpr char kMaximumWidth[] = "maximumWidth";
	constexpr char kMaximumHeight[] = "maximumHeight";
	constexpr char kMaximumFps[] = "maximumFps";
	constexpr char kMaximumBitrateKbps[] = "maximumBitrateKbps";
	constexpr char kClipboardText[] = "clipboardText";
	constexpr char kKeyboard[] = "keyboard";
	constexpr char kUnicodeText[] = "unicodeText";
	constexpr char kMouseButtons[] = "mouseButtons";
	constexpr char kMouseWheel[] = "mouseWheel";
	constexpr char kMonitorList[] = "monitorList";
	constexpr char kId[] = "id";
	constexpr char kPrimary[] = "primary";

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

	bool isValidStreamConfig(const KStreamConfig &config)
	{
		const bool bUsesNativeSize = config.nWidth == 0 && config.nHeight == 0;
		const bool bUsesBoundedSize = config.nWidth >= 2
			&& config.nWidth <= KProtocolConstraints::kMaximumStreamWidth
			&& config.nHeight >= 2
			&& config.nHeight <= KProtocolConstraints::kMaximumStreamHeight;
		return (bUsesNativeSize || bUsesBoundedSize)
			&& config.nFps >= KProtocolConstraints::kMinimumStreamFps
			&& config.nFps <= KProtocolConstraints::kMaximumStreamFps
			&& config.nBitrateKbps >= KProtocolConstraints::kMinimumStreamBitrateKbps
			&& config.nBitrateKbps <= KProtocolConstraints::kMaximumStreamBitrateKbps;
	}

	bool isValidWallpaper(const QString &strMime, const QString &strData)
	{
		if (strMime.isEmpty() && strData.isEmpty())
			return true;
		if (strMime.isEmpty() || strData.isEmpty()
			|| strMime.size() > 64
			|| strData.toUtf8().size() > KProtocolConstraints::kMaximumWallpaperBase64Bytes)
		{
			return false;
		}
		const QByteArray::FromBase64Result result = QByteArray::fromBase64Encoding(
			strData.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
		return bool(result);
	}

	QJsonArray stringArray(const QStringList &values)
	{
		QJsonArray array;
		for (const QString &strValue : values)
			array.append(strValue);
		return array;
	}

	bool readStringArray(const QJsonObject &object, const char *pName, QStringList *pValues)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isArray() || value.toArray().size() > 16)
			return false;
		QStringList values;
		for (const QJsonValue &item : value.toArray())
		{
			if (!item.isString() || item.toString().isEmpty() || item.toString().size() > 32)
				return false;
			values.append(item.toString().toLower());
		}
		values.removeDuplicates();
		*pValues = values;
		return true;
	}

	bool readRequiredBool(const QJsonObject &object, const char *pName, bool *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isBool())
			return false;
		*pValue = value.toBool();
		return true;
	}
}

QString KSessionMessageCodec::encode(const KSessionMessage &message)
{
	QJsonObject object;
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
	else if (message.type == CapabilitiesSessionMessageType)
	{
		const KSessionCapabilities &capabilities = message.capabilities;
		object.insert(QString::fromLatin1(kProtocolMinVersion), capabilities.nProtocolMinVersion);
		object.insert(QString::fromLatin1(kProtocolMaxVersion), capabilities.nProtocolMaxVersion);
		object.insert(QString::fromLatin1(kSupportedCodecs), stringArray(capabilities.supportedCodecs));
		object.insert(QString::fromLatin1(kSupportedChannels), stringArray(capabilities.supportedChannels));
		object.insert(QString::fromLatin1(kMaximumWidth), capabilities.nMaximumWidth);
		object.insert(QString::fromLatin1(kMaximumHeight), capabilities.nMaximumHeight);
		object.insert(QString::fromLatin1(kMaximumFps), capabilities.nMaximumFps);
		object.insert(QString::fromLatin1(kMaximumBitrateKbps), capabilities.nMaximumBitrateKbps);
		object.insert(QString::fromLatin1(kClipboardText), capabilities.bClipboardText);
		object.insert(QString::fromLatin1(kKeyboard), capabilities.bKeyboard);
		object.insert(QString::fromLatin1(kUnicodeText), capabilities.bUnicodeText);
		object.insert(QString::fromLatin1(kMouseButtons), capabilities.bMouseButtons);
		object.insert(QString::fromLatin1(kMouseWheel), capabilities.bMouseWheel);
		QJsonArray monitors;
		for (const KMonitorCapability &monitor : capabilities.monitorList)
		{
			QJsonObject monitorObject;
			monitorObject.insert(QString::fromLatin1(kId), monitor.strId);
			monitorObject.insert(QString::fromLatin1(kWidth), monitor.nWidth);
			monitorObject.insert(QString::fromLatin1(kHeight), monitor.nHeight);
			monitorObject.insert(QString::fromLatin1(kPrimary), monitor.bPrimary);
			monitors.append(monitorObject);
		}
		object.insert(QString::fromLatin1(kMonitorList), monitors);
	}

	return KProtocolEnvelopeCodec::encode(SessionProtocolChannel,
		typeName(message.type), QString(), 0, object);
}

bool KSessionMessageCodec::decode(const QString &strMessage,
	KSessionMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return failDecode(QStringLiteral("Session message output is null"), pErrorMessage);
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	if (envelope.nVersion != KProtocolConstraints::kEnvelopeSchemaVersion)
		return failDecode(QStringLiteral("Unsupported session envelope version"), pErrorMessage);
	return decode(envelope, pMessage, pErrorMessage);
}

bool KSessionMessageCodec::decode(const KProtocolEnvelope &envelope,
	KSessionMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return failDecode(QStringLiteral("Session message output is null"), pErrorMessage);
	const QJsonObject object = envelope.payload;
	const QString strType = envelope.strType;
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
	else if (strType == QString::fromLatin1(kCapabilities))
		message.type = CapabilitiesSessionMessageType;
	else if (strType == QString::fromLatin1(kCapabilityRejected))
		message.type = CapabilityRejectedSessionMessageType;
	else
		return failDecode(QStringLiteral("Unknown session message type"), pErrorMessage);

	if (message.type == DeviceInfoSessionMessageType)
	{
		const QJsonValue computerNameValue = object.value(QString::fromLatin1(kComputerName));
		const QJsonValue wallpaperMimeValue = object.value(QString::fromLatin1(kWallpaperMime));
		const QJsonValue wallpaperDataValue = object.value(QString::fromLatin1(kWallpaperData));
		if (!computerNameValue.isString()
			|| computerNameValue.toString().trimmed().isEmpty()
			|| computerNameValue.toString().size()
				> KProtocolConstraints::kMaximumComputerNameCharacters
			|| !readRequiredInt(object, kScreenWidth, &message.deviceInfo.nScreenWidth)
			|| !readRequiredInt(object, kScreenHeight, &message.deviceInfo.nScreenHeight)
			|| message.deviceInfo.nScreenWidth <= 0
			|| message.deviceInfo.nScreenWidth > KProtocolConstraints::kMaximumScreenDimension
			|| message.deviceInfo.nScreenHeight <= 0
			|| message.deviceInfo.nScreenHeight > KProtocolConstraints::kMaximumScreenDimension
			|| (!wallpaperMimeValue.isUndefined() && !wallpaperMimeValue.isString())
			|| (!wallpaperDataValue.isUndefined() && !wallpaperDataValue.isString())
			|| !isValidWallpaper(wallpaperMimeValue.toString(), wallpaperDataValue.toString()))
		{
			return failDecode(QStringLiteral("Invalid device info message"), pErrorMessage);
		}

		message.deviceInfo.strComputerName = computerNameValue.toString().trimmed();
		message.deviceInfo.strWallpaperMime = wallpaperMimeValue.toString();
		message.deviceInfo.strWallpaperData = wallpaperDataValue.toString();
	}
	else if (message.type == EndSessionMessageType)
	{
		const QJsonValue reasonValue = object.value(QString::fromLatin1(kReason));
		if ((!reasonValue.isUndefined() && !reasonValue.isString())
			|| reasonValue.toString().size() > KProtocolConstraints::kMaximumReasonCharacters)
			return failDecode(QStringLiteral("Invalid end session message"), pErrorMessage);
		message.strReason = reasonValue.toString();
	}
	else if (message.type == StreamConfigSessionMessageType)
	{
		if (!readOptionalInt(object, kFps, &message.streamConfig.nFps)
			|| !readOptionalInt(object, kWidth, &message.streamConfig.nWidth)
			|| !readOptionalInt(object, kHeight, &message.streamConfig.nHeight)
			|| !readOptionalInt(object, kBitrateKbps, &message.streamConfig.nBitrateKbps)
			|| !isValidStreamConfig(message.streamConfig))
		{
			return failDecode(QStringLiteral("Invalid stream config message"), pErrorMessage);
		}
	}
	else if (message.type == CapabilitiesSessionMessageType)
	{
		KSessionCapabilities &capabilities = message.capabilities;
		const QJsonValue monitorsValue = object.value(QString::fromLatin1(kMonitorList));
		if (!readRequiredInt(object, kProtocolMinVersion, &capabilities.nProtocolMinVersion)
			|| !readRequiredInt(object, kProtocolMaxVersion, &capabilities.nProtocolMaxVersion)
			|| capabilities.nProtocolMinVersion < 1
			|| capabilities.nProtocolMaxVersion < capabilities.nProtocolMinVersion
			|| capabilities.nProtocolMaxVersion > 100
			|| !readStringArray(object, kSupportedCodecs, &capabilities.supportedCodecs)
			|| !readStringArray(object, kSupportedChannels, &capabilities.supportedChannels)
			|| !readRequiredInt(object, kMaximumWidth, &capabilities.nMaximumWidth)
			|| !readRequiredInt(object, kMaximumHeight, &capabilities.nMaximumHeight)
			|| !readRequiredInt(object, kMaximumFps, &capabilities.nMaximumFps)
			|| !readRequiredInt(object, kMaximumBitrateKbps, &capabilities.nMaximumBitrateKbps)
			|| capabilities.nMaximumWidth < 2 || capabilities.nMaximumWidth > KProtocolConstraints::kMaximumStreamWidth
			|| capabilities.nMaximumHeight < 2 || capabilities.nMaximumHeight > KProtocolConstraints::kMaximumStreamHeight
			|| capabilities.nMaximumFps < 1 || capabilities.nMaximumFps > KProtocolConstraints::kMaximumStreamFps
			|| capabilities.nMaximumBitrateKbps < KProtocolConstraints::kMinimumStreamBitrateKbps
			|| capabilities.nMaximumBitrateKbps > KProtocolConstraints::kMaximumStreamBitrateKbps
			|| !readRequiredBool(object, kClipboardText, &capabilities.bClipboardText)
			|| !readRequiredBool(object, kKeyboard, &capabilities.bKeyboard)
			|| !readRequiredBool(object, kUnicodeText, &capabilities.bUnicodeText)
			|| !readRequiredBool(object, kMouseButtons, &capabilities.bMouseButtons)
			|| !readRequiredBool(object, kMouseWheel, &capabilities.bMouseWheel)
			|| !monitorsValue.isArray() || monitorsValue.toArray().size() > 16)
		{
			return failDecode(QStringLiteral("Invalid session capabilities"), pErrorMessage);
		}
		for (const QJsonValue &monitorValue : monitorsValue.toArray())
		{
			if (!monitorValue.isObject())
				return failDecode(QStringLiteral("Invalid monitor capability"), pErrorMessage);
			const QJsonObject monitorObject = monitorValue.toObject();
			KMonitorCapability monitor;
			const QJsonValue idValue = monitorObject.value(QString::fromLatin1(kId));
			const QJsonValue primaryValue = monitorObject.value(QString::fromLatin1(kPrimary));
			if (!idValue.isString() || idValue.toString().isEmpty() || idValue.toString().size() > 64
				|| !readRequiredInt(monitorObject, kWidth, &monitor.nWidth)
				|| !readRequiredInt(monitorObject, kHeight, &monitor.nHeight)
				|| monitor.nWidth < 2 || monitor.nHeight < 2
				|| !primaryValue.isBool())
			{
				return failDecode(QStringLiteral("Invalid monitor capability"), pErrorMessage);
			}
			monitor.strId = idValue.toString();
			monitor.bPrimary = primaryValue.toBool();
			capabilities.monitorList.append(monitor);
		}
	}
	else if (message.type == CapabilityRejectedSessionMessageType)
	{
		const QJsonValue reasonValue = object.value(QString::fromLatin1(kReason));
		if (!reasonValue.isString() || reasonValue.toString().isEmpty()
			|| reasonValue.toString().size() > KProtocolConstraints::kMaximumReasonCharacters)
		{
			return failDecode(QStringLiteral("Invalid capability rejection"), pErrorMessage);
		}
		message.strReason = reasonValue.toString();
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
	if (type == CapabilitiesSessionMessageType)
		return QString::fromLatin1(kCapabilities);
	if (type == CapabilityRejectedSessionMessageType)
		return QString::fromLatin1(kCapabilityRejected);
	return QStringLiteral("invalid");
}

bool KSessionMessageCodec::negotiate(const KSessionCapabilities &local,
	const KSessionCapabilities &remote,
	KNegotiatedCapabilities *pNegotiated,
	QString *pErrorMessage)
{
	if (pNegotiated == nullptr)
		return failDecode(QStringLiteral("Negotiated capability output is null"), pErrorMessage);
	const int nMinimumVersion = std::max(local.nProtocolMinVersion, remote.nProtocolMinVersion);
	const int nMaximumVersion = std::min(local.nProtocolMaxVersion, remote.nProtocolMaxVersion);
	if (nMinimumVersion > nMaximumVersion)
		return failDecode(QStringLiteral("No compatible session protocol version"), pErrorMessage);
	if (!local.supportedCodecs.contains(QStringLiteral("h264"))
		|| !remote.supportedCodecs.contains(QStringLiteral("h264")))
	{
		return failDecode(QStringLiteral("H.264 is not supported by both peers"), pErrorMessage);
	}
	for (const QString &strRequiredChannel : { QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input") })
	{
		if (!local.supportedChannels.contains(strRequiredChannel)
			|| !remote.supportedChannels.contains(strRequiredChannel))
		{
			return failDecode(QStringLiteral("Missing required channel: %1").arg(strRequiredChannel), pErrorMessage);
		}
	}

	KNegotiatedCapabilities negotiated;
	negotiated.bValid = true;
	negotiated.nProtocolVersion = nMaximumVersion;
	negotiated.strVideoCodec = QStringLiteral("h264");
	for (const QString &strChannel : local.supportedChannels)
	{
		if (remote.supportedChannels.contains(strChannel))
			negotiated.channels.append(strChannel);
	}
	negotiated.nMaximumWidth = std::min(local.nMaximumWidth, remote.nMaximumWidth);
	negotiated.nMaximumHeight = std::min(local.nMaximumHeight, remote.nMaximumHeight);
	negotiated.nMaximumFps = std::min(local.nMaximumFps, remote.nMaximumFps);
	negotiated.nMaximumBitrateKbps = std::min(local.nMaximumBitrateKbps, remote.nMaximumBitrateKbps);
	negotiated.bClipboardText = local.bClipboardText && remote.bClipboardText
		&& negotiated.channels.contains(QStringLiteral("clipboard"));
	negotiated.bKeyboard = local.bKeyboard && remote.bKeyboard;
	negotiated.bUnicodeText = local.bUnicodeText && remote.bUnicodeText;
	negotiated.bMouseButtons = local.bMouseButtons && remote.bMouseButtons;
	negotiated.bMouseWheel = local.bMouseWheel && remote.bMouseWheel;
	*pNegotiated = negotiated;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}
