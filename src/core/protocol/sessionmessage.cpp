#include "core/protocol/sessionmessage.h"

#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

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
	constexpr char kCommandResult[] = "commandResult";
	constexpr char kSetPrivacyMode[] = "setPrivacyMode";
	constexpr char kPrivacyModeState[] = "privacyModeState";
	constexpr char kSetPostSessionAction[] = "setPostSessionAction";
	constexpr char kPostSessionActionState[] = "postSessionActionState";
	constexpr char kSuccess[] = "success";
	constexpr char kErrorCode[] = "errorCode";
	constexpr char kProtocolMinVersion[] = "protocolMinVersion";
	constexpr char kProtocolMaxVersion[] = "protocolMaxVersion";
	constexpr char kSupportedCodecs[] = "supportedCodecs";
	constexpr char kSupportedChannels[] = "supportedChannels";
	constexpr char kMaximumWidth[] = "maximumWidth";
	constexpr char kMaximumHeight[] = "maximumHeight";
	constexpr char kMaximumFps[] = "maximumFps";
	constexpr char kMaximumBitrateKbps[] = "maximumBitrateKbps";
	constexpr char kClipboardText[] = "clipboardText";
	constexpr char kInputRealtime[] = "inputRealtime";
	constexpr char kKeyboard[] = "keyboard";
	constexpr char kUnicodeText[] = "unicodeText";
	constexpr char kMouseButtons[] = "mouseButtons";
	constexpr char kMouseWheel[] = "mouseWheel";
	constexpr char kSupportedPrivacyModes[] = "supportedPrivacyModes";
	constexpr char kPostSessionLock[] = "postSessionLock";
	constexpr char kPrivacyMode[] = "privacyMode";
	constexpr char kRequestedMode[] = "requestedMode";
	constexpr char kEffectiveMode[] = "effectiveMode";
	constexpr char kPrivacyState[] = "privacyState";
	constexpr char kPostSessionAction[] = "postSessionAction";
	constexpr char kGeneration[] = "generation";
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

	bool readOptionalBool(const QJsonObject &object, const char *pName, bool *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (value.isUndefined())
			return true;
		if (!value.isBool())
			return false;
		*pValue = value.toBool();
		return true;
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

	bool readOptionalStringArray(const QJsonObject &object,
		const char *pName,
		QStringList *pValues)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (value.isUndefined())
		{
			pValues->clear();
			return true;
		}
		return readStringArray(object, pName, pValues);
	}

	QString privacyModeName(KPrivacyMode mode)
	{
		if (mode == DisabledPrivacyMode)
			return QStringLiteral("disabled");
		if (mode == PrivacyOverlayPrivacyMode)
			return QStringLiteral("privacyoverlay");
		if (mode == DisplayOffPrivacyMode)
			return QStringLiteral("displayoff");
		return QStringLiteral("unknown");
	}

	KPrivacyMode privacyModeFromName(const QString &strName)
	{
		const QString strNormalized = strName.toLower();
		if (strNormalized == QStringLiteral("disabled"))
			return DisabledPrivacyMode;
		if (strNormalized == QStringLiteral("privacyoverlay"))
			return PrivacyOverlayPrivacyMode;
		if (strNormalized == QStringLiteral("displayoff"))
			return DisplayOffPrivacyMode;
		return UnknownPrivacyMode;
	}

	QString privacyStateName(KPrivacyModeState state)
	{
		if (state == InactivePrivacyModeState)
			return QStringLiteral("inactive");
		if (state == ApplyingPrivacyModeState)
			return QStringLiteral("applying");
		if (state == ActivePrivacyModeState)
			return QStringLiteral("active");
		if (state == RestoringPrivacyModeState)
			return QStringLiteral("restoring");
		if (state == FailedPrivacyModeState)
			return QStringLiteral("failed");
		return QStringLiteral("failed");
	}

	bool privacyStateFromName(const QString &strName, KPrivacyModeState *pState)
	{
		if (strName == QStringLiteral("inactive"))
			*pState = InactivePrivacyModeState;
		else if (strName == QStringLiteral("applying"))
			*pState = ApplyingPrivacyModeState;
		else if (strName == QStringLiteral("active"))
			*pState = ActivePrivacyModeState;
		else if (strName == QStringLiteral("restoring"))
			*pState = RestoringPrivacyModeState;
		else if (strName == QStringLiteral("failed"))
			*pState = FailedPrivacyModeState;
		else
			return false;
		return true;
	}

	QString postSessionActionName(KPostSessionAction action)
	{
		if (action == NoPostSessionAction)
			return QStringLiteral("none");
		if (action == LockWorkstationPostSessionAction)
			return QStringLiteral("lockworkstation");
		return QStringLiteral("unknown");
	}

	KPostSessionAction postSessionActionFromName(const QString &strName)
	{
		const QString strNormalized = strName.toLower();
		if (strNormalized == QStringLiteral("none"))
			return NoPostSessionAction;
		if (strNormalized == QStringLiteral("lockworkstation"))
			return LockWorkstationPostSessionAction;
		return UnknownPostSessionAction;
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
	else if (message.type == StreamConfigSessionMessageType
		|| (message.type == StartStreamingSessionMessageType && message.bHasStreamConfig))
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
		object.insert(QString::fromLatin1(kInputRealtime), capabilities.bInputRealtime);
		object.insert(QString::fromLatin1(kKeyboard), capabilities.bKeyboard);
		object.insert(QString::fromLatin1(kUnicodeText), capabilities.bUnicodeText);
		object.insert(QString::fromLatin1(kMouseButtons), capabilities.bMouseButtons);
		object.insert(QString::fromLatin1(kMouseWheel), capabilities.bMouseWheel);
		object.insert(QString::fromLatin1(kSupportedPrivacyModes),
			stringArray(capabilities.supportedPrivacyModes));
		object.insert(QString::fromLatin1(kPostSessionLock), capabilities.bPostSessionLock);
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
	else if (message.type == SetPrivacyModeSessionMessageType)
	{
		object.insert(QString::fromLatin1(kPrivacyMode), privacyModeName(message.privacyMode));
	}
	else if (message.type == PrivacyModeStateSessionMessageType)
	{
		const KPrivacyModeStatus &status = message.privacyModeStatus;
		object.insert(QString::fromLatin1(kRequestedMode), privacyModeName(status.requestedMode));
		object.insert(QString::fromLatin1(kEffectiveMode), privacyModeName(status.effectiveMode));
		object.insert(QString::fromLatin1(kPrivacyState), privacyStateName(status.state));
		object.insert(QString::fromLatin1(kGeneration), QString::number(status.nGeneration));
		if (!status.strErrorCode.isEmpty())
			object.insert(QString::fromLatin1(kErrorCode), status.strErrorCode);
	}
	else if (message.type == SetPostSessionActionSessionMessageType)
	{
		object.insert(QString::fromLatin1(kPostSessionAction),
			postSessionActionName(message.postSessionAction));
	}
	else if (message.type == PostSessionActionStateSessionMessageType)
	{
		const KPostSessionActionStatus &status = message.postSessionActionStatus;
		object.insert(QString::fromLatin1(kPostSessionAction),
			postSessionActionName(status.action));
		object.insert(QString::fromLatin1(kGeneration), QString::number(status.nGeneration));
		if (!status.strErrorCode.isEmpty())
			object.insert(QString::fromLatin1(kErrorCode), status.strErrorCode);
	}
	else if (message.type == CommandResultSessionMessageType)
	{
		object.insert(QString::fromLatin1(kSuccess), message.bSuccess);
		if (!message.bSuccess)
			object.insert(QString::fromLatin1(kErrorCode), message.strErrorCode);
	}

	return KProtocolEnvelopeCodec::encode(SessionProtocolChannel,
		typeName(message.type), message.strRequestId, 0, object);
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
	else if (strType == QString::fromLatin1(kSetPrivacyMode))
		message.type = SetPrivacyModeSessionMessageType;
	else if (strType == QString::fromLatin1(kPrivacyModeState))
		message.type = PrivacyModeStateSessionMessageType;
	else if (strType == QString::fromLatin1(kSetPostSessionAction))
		message.type = SetPostSessionActionSessionMessageType;
	else if (strType == QString::fromLatin1(kPostSessionActionState))
		message.type = PostSessionActionStateSessionMessageType;
	else if (strType == QString::fromLatin1(kCommandResult))
		message.type = CommandResultSessionMessageType;
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
	else if (message.type == StartStreamingSessionMessageType
		&& (!object.value(QString::fromLatin1(kFps)).isUndefined()
			|| !object.value(QString::fromLatin1(kWidth)).isUndefined()
			|| !object.value(QString::fromLatin1(kHeight)).isUndefined()
			|| !object.value(QString::fromLatin1(kBitrateKbps)).isUndefined()))
	{
		if (!readRequiredInt(object, kFps, &message.streamConfig.nFps)
			|| !readRequiredInt(object, kWidth, &message.streamConfig.nWidth)
			|| !readRequiredInt(object, kHeight, &message.streamConfig.nHeight)
			|| !readRequiredInt(object, kBitrateKbps, &message.streamConfig.nBitrateKbps)
			|| !isValidStreamConfig(message.streamConfig))
		{
			return failDecode(QStringLiteral("Invalid atomic start stream config"), pErrorMessage);
		}
		message.bHasStreamConfig = true;
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
			|| !readOptionalBool(object, kInputRealtime, &capabilities.bInputRealtime)
			|| !readRequiredBool(object, kKeyboard, &capabilities.bKeyboard)
			|| !readRequiredBool(object, kUnicodeText, &capabilities.bUnicodeText)
			|| !readRequiredBool(object, kMouseButtons, &capabilities.bMouseButtons)
			|| !readRequiredBool(object, kMouseWheel, &capabilities.bMouseWheel)
			|| !readOptionalStringArray(object, kSupportedPrivacyModes,
				&capabilities.supportedPrivacyModes)
			|| !readOptionalBool(object, kPostSessionLock, &capabilities.bPostSessionLock)
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
	else if (message.type == SetPrivacyModeSessionMessageType)
	{
		const QJsonValue modeValue = object.value(QString::fromLatin1(kPrivacyMode));
		if (!modeValue.isString() || modeValue.toString().isEmpty()
			|| modeValue.toString().size() > 32)
		{
			return failDecode(QStringLiteral("Invalid privacy mode command"), pErrorMessage);
		}
		message.privacyMode = privacyModeFromName(modeValue.toString());
	}
	else if (message.type == PrivacyModeStateSessionMessageType)
	{
		const QJsonValue requestedValue = object.value(QString::fromLatin1(kRequestedMode));
		const QJsonValue effectiveValue = object.value(QString::fromLatin1(kEffectiveMode));
		const QJsonValue stateValue = object.value(QString::fromLatin1(kPrivacyState));
		const QJsonValue generationValue = object.value(QString::fromLatin1(kGeneration));
		bool bGenerationOk = false;
		const quint64 nGeneration = generationValue.toString().toULongLong(&bGenerationOk);
		if (!requestedValue.isString() || !effectiveValue.isString()
			|| !stateValue.isString() || !generationValue.isString() || !bGenerationOk
			|| requestedValue.toString().size() > 32
			|| effectiveValue.toString().size() > 32
			|| !privacyStateFromName(stateValue.toString(), &message.privacyModeStatus.state))
		{
			return failDecode(QStringLiteral("Invalid privacy mode state"), pErrorMessage);
		}
		message.privacyModeStatus.requestedMode = privacyModeFromName(requestedValue.toString());
		message.privacyModeStatus.effectiveMode = privacyModeFromName(effectiveValue.toString());
		if (message.privacyModeStatus.requestedMode == UnknownPrivacyMode
			|| message.privacyModeStatus.effectiveMode == UnknownPrivacyMode)
		{
			return failDecode(QStringLiteral("Unknown privacy mode state"), pErrorMessage);
		}
		message.privacyModeStatus.nGeneration = nGeneration;
		message.privacyModeStatus.strRequestId = envelope.strRequestId;
		message.privacyModeStatus.strErrorCode = object.value(
			QString::fromLatin1(kErrorCode)).toString();
	}
	else if (message.type == SetPostSessionActionSessionMessageType)
	{
		const QJsonValue actionValue = object.value(QString::fromLatin1(kPostSessionAction));
		if (!actionValue.isString() || actionValue.toString().isEmpty()
			|| actionValue.toString().size() > 32)
		{
			return failDecode(QStringLiteral("Invalid post session action command"), pErrorMessage);
		}
		message.postSessionAction = postSessionActionFromName(actionValue.toString());
	}
	else if (message.type == PostSessionActionStateSessionMessageType)
	{
		const QJsonValue actionValue = object.value(QString::fromLatin1(kPostSessionAction));
		const QJsonValue generationValue = object.value(QString::fromLatin1(kGeneration));
		bool bGenerationOk = false;
		const quint64 nGeneration = generationValue.toString().toULongLong(&bGenerationOk);
		if (!actionValue.isString() || actionValue.toString().size() > 32
			|| !generationValue.isString() || !bGenerationOk)
		{
			return failDecode(QStringLiteral("Invalid post session action state"), pErrorMessage);
		}
		message.postSessionActionStatus.action = postSessionActionFromName(actionValue.toString());
		if (message.postSessionActionStatus.action == UnknownPostSessionAction)
			return failDecode(QStringLiteral("Unknown post session action state"), pErrorMessage);
		message.postSessionActionStatus.nGeneration = nGeneration;
		message.postSessionActionStatus.strRequestId = envelope.strRequestId;
		message.postSessionActionStatus.strErrorCode = object.value(
			QString::fromLatin1(kErrorCode)).toString();
	}
	else if (message.type == CommandResultSessionMessageType)
	{
		const QJsonValue successValue = object.value(QString::fromLatin1(kSuccess));
		const QJsonValue errorCodeValue = object.value(QString::fromLatin1(kErrorCode));
		if (envelope.strRequestId.isEmpty()
			|| !successValue.isBool()
			|| (!successValue.toBool()
				&& (!errorCodeValue.isString()
					|| errorCodeValue.toString().isEmpty()
					|| errorCodeValue.toString().size() > 64)))
		{
			return failDecode(QStringLiteral("Invalid session command result"), pErrorMessage);
		}
		message.bSuccess = successValue.toBool();
		message.strErrorCode = errorCodeValue.toString();
	}

	message.strRequestId = envelope.strRequestId;
	if (isCommand(message.type) && message.strRequestId.isEmpty())
		return failDecode(QStringLiteral("Session command request id is required"), pErrorMessage);

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
	if (type == SetPrivacyModeSessionMessageType)
		return QString::fromLatin1(kSetPrivacyMode);
	if (type == PrivacyModeStateSessionMessageType)
		return QString::fromLatin1(kPrivacyModeState);
	if (type == SetPostSessionActionSessionMessageType)
		return QString::fromLatin1(kSetPostSessionAction);
	if (type == PostSessionActionStateSessionMessageType)
		return QString::fromLatin1(kPostSessionActionState);
	if (type == CommandResultSessionMessageType)
		return QString::fromLatin1(kCommandResult);
	return QStringLiteral("invalid");
}

bool KSessionMessageCodec::isCommand(KSessionMessageType type)
{
	return type == DeviceInfoRequestSessionMessageType
		|| type == StartStreamingSessionMessageType
		|| type == StopStreamingSessionMessageType
		|| type == EndSessionMessageType
		|| type == StreamConfigSessionMessageType
		|| type == SetPrivacyModeSessionMessageType
		|| type == SetPostSessionActionSessionMessageType;
}
