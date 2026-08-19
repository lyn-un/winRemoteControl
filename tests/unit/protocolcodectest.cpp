#include "core/protocol/accessmessage.h"
#include "core/protocol/clipboardmessage.h"
#include "core/protocol/inputmessage.h"
#include "core/protocol/landiscoverymessage.h"
#include "core/protocol/protocolconstraints.h"
#include "core/protocol/sessionmessage.h"
#include "core/protocol/terminalmessage.h"
#include "core/protocol/terminaldataframe.h"
#include "core/protocol/tlspairingmessage.h"
#include "core/protocol/webrtcsignalingmessage.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;

		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	void testMouseMoveRoundTrip()
	{
		KInputMessage source;
		source.type = MouseMoveInputMessageType;
		source.nX = 321;
		source.nY = 654;
		source.nSequence = 42;
		source.bTrace = true;

		KInputMessage decoded;
		QString strError;
		check(KInputMessageCodec::decode(KInputMessageCodec::encode(source), &decoded, &strError),
			QStringLiteral("mouse move decodes"));
		check(decoded.type == source.type, QStringLiteral("mouse move type round-trips"));
		check(decoded.nX == source.nX && decoded.nY == source.nY,
			QStringLiteral("mouse move coordinates round-trip"));
		check(decoded.nSequence == source.nSequence && decoded.bTrace,
			QStringLiteral("mouse move metadata round-trips"));
	}

	void testMouseButtonRoundTrip()
	{
		KInputMessage source;
		source.type = MouseButtonInputMessageType;
		source.mouseButton = RightRemoteMouseButton;
		source.nX = 10;
		source.nY = 20;
		source.bPressed = true;

		KInputMessage decoded;
		check(KInputMessageCodec::decode(KInputMessageCodec::encode(source), &decoded, nullptr),
			QStringLiteral("mouse button decodes"));
		check(decoded.mouseButton == RightRemoteMouseButton && decoded.bPressed,
			QStringLiteral("mouse button payload round-trips"));
	}

	void testMouseWheelRoundTrip()
	{
		KInputMessage source;
		source.type = MouseWheelInputMessageType;
		source.nX = 12;
		source.nY = 34;
		source.nWheelDelta = -120;

		KInputMessage decoded;
		check(KInputMessageCodec::decode(KInputMessageCodec::encode(source), &decoded, nullptr),
			QStringLiteral("mouse wheel decodes"));
		check(decoded.nWheelDelta == -120, QStringLiteral("mouse wheel delta round-trips"));
	}

	void testKeyRoundTrip()
	{
		KInputMessage source;
		source.type = KeyInputMessageType;
		source.nVirtualKey = 0x25;
		source.bPressed = false;
		source.bExtended = true;
		source.nScanCode = 0x14D;
		source.bAutoRepeat = true;
		source.nSequence = 99;

		KInputMessage decoded;
		check(KInputMessageCodec::decode(KInputMessageCodec::encode(source), &decoded, nullptr),
			QStringLiteral("key input decodes"));
		check(decoded.nVirtualKey == source.nVirtualKey
			&& !decoded.bPressed
			&& decoded.bExtended
			&& decoded.nScanCode == source.nScanCode
			&& decoded.bAutoRepeat,
			QStringLiteral("key payload round-trips"));
	}

	void testInvalidInputMessages()
	{
		KInputMessage message;
		check(!KInputMessageCodec::decode(QStringLiteral("not-json"), &message, nullptr),
			QStringLiteral("non-JSON input is rejected"));
		check(!KInputMessageCodec::decode(QStringLiteral("{\"type\":\"unknown\"}"), &message, nullptr),
			QStringLiteral("unknown input type is rejected"));
		check(!KInputMessageCodec::decode(
			QStringLiteral("{\"type\":\"key\",\"vk\":0,\"pressed\":true}"), &message, nullptr),
			QStringLiteral("out-of-range key is rejected"));
		check(!KInputMessageCodec::decode(
			QStringLiteral("{\"type\":\"mouseButton\",\"button\":\"unknown\",\"pressed\":true,\"x\":1,\"y\":2}"),
			&message,
			nullptr),
			QStringLiteral("unknown mouse button is rejected"));
		check(!KInputMessageCodec::decode(
			QStringLiteral("{\"version\":2,\"type\":\"mouseMove\",\"x\":1,\"y\":2}"),
			&message, nullptr),
			QStringLiteral("unsupported input version is rejected"));
		check(!KInputMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"mouseMove\",\"x\":-1,\"y\":2}"),
			&message, nullptr),
			QStringLiteral("out-of-range mouse coordinate is rejected"));
		check(!KInputMessageCodec::decode(
			QString(KProtocolConstraints::kMaximumInputMessageBytes + 1, QLatin1Char('x')),
			&message, nullptr),
			QStringLiteral("oversized input message is rejected"));
	}

	void testExtendedInputRoundTrip()
	{
		for (KRemoteMouseButton button : { MiddleRemoteMouseButton,
			X1RemoteMouseButton, X2RemoteMouseButton })
		{
			KInputMessage source;
			source.type = MouseButtonInputMessageType;
			source.mouseButton = button;
			source.bPressed = true;
			source.nX = 100;
			source.nY = 200;
			KInputMessage decoded;
			check(KInputMessageCodec::decode(KInputMessageCodec::encode(source),
					&decoded, nullptr) && decoded.mouseButton == button,
				QStringLiteral("extended mouse button round-trips"));
		}

		KInputMessage text;
		text.type = TextInputMessageType;
		text.strText = QString::fromUtf8("中文🙂");
		KInputMessage decoded;
		check(KInputMessageCodec::decode(KInputMessageCodec::encode(text), &decoded, nullptr)
			&& decoded.strText == text.strText,
			QStringLiteral("Unicode text input round-trips"));
		text.strText = QString(KProtocolConstraints::kMaximumTextInputBytes + 1, QLatin1Char('x'));
		check(!KInputMessageCodec::decode(KInputMessageCodec::encode(text), &decoded, nullptr),
			QStringLiteral("oversized Unicode input is rejected"));
	}

	void testLegacyInputWireCompatibility()
	{
		const QString strLegacyMouse = QStringLiteral(
			"{\"version\":1,\"type\":\"mouseButton\",\"button\":\"left\",\"pressed\":true,"
			"\"x\":11,\"y\":22,\"seq\":\"7\",\"trace\":true}");
		KInputMessage mouseMessage;
		check(KInputMessageCodec::decode(strLegacyMouse, &mouseMessage, nullptr),
			QStringLiteral("flat mouse JSON decodes"));
		check(mouseMessage.type == MouseButtonInputMessageType
			&& mouseMessage.mouseButton == LeftRemoteMouseButton
			&& mouseMessage.nSequence == 7,
			QStringLiteral("flat mouse fields retain their meaning"));

		KInputMessage keyMessage;
		keyMessage.type = KeyInputMessageType;
		keyMessage.nVirtualKey = 65;
		keyMessage.bPressed = true;
		keyMessage.bExtended = false;
		keyMessage.nSequence = 8;
		const QJsonObject keyObject = QJsonDocument::fromJson(
			KInputMessageCodec::encode(keyMessage).toUtf8()).object();
		check(keyObject.value(QStringLiteral("type")).toString() == QStringLiteral("key")
			&& keyObject.value(QStringLiteral("vk")).toInt() == 65
			&& keyObject.value(QStringLiteral("pressed")).toBool()
			&& keyObject.value(QStringLiteral("seq")).toString() == QStringLiteral("8"),
			QStringLiteral("encoded key JSON keeps legacy field names and types"));
	}

	void testSessionControlRoundTrip()
	{
		const KSessionMessageType types[] = {
			DeviceInfoRequestSessionMessageType,
			StartStreamingSessionMessageType,
			StopStreamingSessionMessageType
		};
		for (const KSessionMessageType type : types)
		{
			KSessionMessage source;
			source.type = type;
			source.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
			KSessionMessage decoded;
			check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(source), &decoded, nullptr),
				QStringLiteral("session control message decodes"));
			check(decoded.type == type, QStringLiteral("session control type round-trips"));
		}
	}

	void testPrivacyControlRoundTrip()
	{
		const QString strRequestId = QStringLiteral(
			"32345678-1234-1234-1234-1234567890ab");
		KSessionMessage modeCommand;
		modeCommand.type = SetPrivacyModeSessionMessageType;
		modeCommand.strRequestId = strRequestId;
		modeCommand.privacyMode = PrivacyOverlayPrivacyMode;
		KSessionMessage decoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(modeCommand),
				&decoded, nullptr)
			&& decoded.type == SetPrivacyModeSessionMessageType
			&& decoded.privacyMode == PrivacyOverlayPrivacyMode,
			QStringLiteral("privacy mode command round-trips"));

		KSessionMessage modeState;
		modeState.type = PrivacyModeStateSessionMessageType;
		modeState.strRequestId = strRequestId;
		modeState.privacyModeStatus.requestedMode = PrivacyOverlayPrivacyMode;
		modeState.privacyModeStatus.effectiveMode = PrivacyOverlayPrivacyMode;
		modeState.privacyModeStatus.state = ActivePrivacyModeState;
		modeState.privacyModeStatus.nGeneration = 42;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(modeState),
				&decoded, nullptr)
			&& decoded.privacyModeStatus.effectiveMode == PrivacyOverlayPrivacyMode
			&& decoded.privacyModeStatus.state == ActivePrivacyModeState
			&& decoded.privacyModeStatus.nGeneration == 42,
			QStringLiteral("privacy mode state round-trips"));

		KSessionMessage actionCommand;
		actionCommand.type = SetPostSessionActionSessionMessageType;
		actionCommand.strRequestId = strRequestId;
		actionCommand.postSessionAction = LockWorkstationPostSessionAction;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(actionCommand),
				&decoded, nullptr)
			&& decoded.postSessionAction == LockWorkstationPostSessionAction,
			QStringLiteral("post session action command round-trips"));

		check(KSessionMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"setPrivacyMode\","
				"\"requestId\":\"32345678-1234-1234-1234-1234567890ab\","
				"\"privacyMode\":\"futuremode\"}"), &decoded, nullptr)
			&& decoded.privacyMode == UnknownPrivacyMode,
			QStringLiteral("unknown privacy command reaches the command handler"));
	}

	void testDeviceInfoRoundTrip()
	{
		KSessionMessage source;
		source.type = DeviceInfoSessionMessageType;
		source.deviceInfo.strComputerName = QStringLiteral("test-host");
		source.deviceInfo.nScreenWidth = 1920;
		source.deviceInfo.nScreenHeight = 1080;
		source.deviceInfo.strWallpaperMime = QStringLiteral("image/jpeg");
		source.deviceInfo.strWallpaperData = QStringLiteral("YWJj");

		KSessionMessage decoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(source), &decoded, nullptr),
			QStringLiteral("device info decodes"));
		check(decoded.deviceInfo.strComputerName == source.deviceInfo.strComputerName,
			QStringLiteral("computer name round-trips"));
		check(decoded.deviceInfo.nScreenWidth == 1920 && decoded.deviceInfo.nScreenHeight == 1080,
			QStringLiteral("screen size round-trips"));
		check(decoded.deviceInfo.strWallpaperData == source.deviceInfo.strWallpaperData,
			QStringLiteral("wallpaper round-trips"));
	}

	void testEndSessionAndStreamConfigRoundTrip()
	{
		KSessionMessage endSource;
		endSource.type = EndSessionMessageType;
		endSource.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		endSource.strReason = QStringLiteral("test_stop");
		KSessionMessage endDecoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(endSource), &endDecoded, nullptr),
			QStringLiteral("end session decodes"));
		check(endDecoded.strReason == endSource.strReason,
			QStringLiteral("end session reason round-trips"));

		KSessionMessage configSource;
		configSource.type = StreamConfigSessionMessageType;
		configSource.strRequestId = QStringLiteral("22345678-1234-1234-1234-1234567890ab");
		configSource.streamConfig.nFps = 60;
		configSource.streamConfig.nWidth = 1600;
		configSource.streamConfig.nHeight = 900;
		configSource.streamConfig.nBitrateKbps = 5000;
		KSessionMessage configDecoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(configSource), &configDecoded, nullptr),
			QStringLiteral("stream config decodes"));
		check(configDecoded.streamConfig.nFps == 60
			&& configDecoded.streamConfig.nWidth == 1600
			&& configDecoded.streamConfig.nHeight == 900
			&& configDecoded.streamConfig.nBitrateKbps == 5000,
			QStringLiteral("stream config round-trips"));

		KSessionMessage resultSource;
		resultSource.type = CommandResultSessionMessageType;
		resultSource.strRequestId = configSource.strRequestId;
		resultSource.bSuccess = false;
		resultSource.strErrorCode = QStringLiteral("invalid_state");
		KSessionMessage resultDecoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(resultSource),
			&resultDecoded, nullptr)
			&& resultDecoded.type == CommandResultSessionMessageType
			&& resultDecoded.strRequestId == resultSource.strRequestId
			&& !resultDecoded.bSuccess
			&& resultDecoded.strErrorCode == resultSource.strErrorCode,
			QStringLiteral("session command result round-trips"));

		KSessionMessage startSource;
		startSource.type = StartStreamingSessionMessageType;
		startSource.strRequestId = QStringLiteral("32345678-1234-1234-1234-1234567890ab");
		startSource.bHasStreamConfig = true;
		startSource.streamConfig = configSource.streamConfig;
		KSessionMessage startDecoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(startSource),
			&startDecoded, nullptr)
			&& startDecoded.bHasStreamConfig
			&& startDecoded.streamConfig.nWidth == configSource.streamConfig.nWidth,
			QStringLiteral("atomic start stream config round-trips"));
	}

	void testInvalidSessionMessages()
	{
		KSessionMessage message;
		check(!KSessionMessageCodec::decode(QStringLiteral("[]"), &message, nullptr),
			QStringLiteral("non-object session message is rejected"));
		check(!KSessionMessageCodec::decode(QStringLiteral("{\"type\":\"unknown\"}"), &message, nullptr),
			QStringLiteral("unknown session type is rejected"));
		check(!KSessionMessageCodec::decode(
			QStringLiteral("{\"type\":\"deviceInfo\",\"computerName\":\"pc\",\"screenWidth\":-1,\"screenHeight\":1080}"),
			&message,
			nullptr),
			QStringLiteral("invalid screen size is rejected"));
		check(!KSessionMessageCodec::decode(
			QStringLiteral("{\"version\":2,\"type\":\"startStreaming\"}"),
			&message, nullptr),
			QStringLiteral("unsupported session version is rejected"));
		check(!KSessionMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"streamConfig\",\"fps\":61,"
				"\"width\":1280,\"height\":720,\"bitrateKbps\":3000}"),
			&message, nullptr),
			QStringLiteral("out-of-range stream config is rejected"));
		check(!KSessionMessageCodec::decode(
			QString(KProtocolConstraints::kMaximumSessionMessageBytes + 1, QLatin1Char('x')),
			&message, nullptr),
			QStringLiteral("oversized session message is rejected"));
		check(!KSessionMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"deviceInfo\",\"computerName\":\"pc\","
				"\"screenWidth\":1920,\"screenHeight\":1080,"
				"\"wallpaperMime\":\"image/jpeg\",\"wallpaperData\":\"not-base64!\"}"),
			&message, nullptr),
			QStringLiteral("malformed wallpaper Base64 is rejected"));
	}

	void testLegacySessionWireCompatibility()
	{
		const QString strLegacyDeviceInfo = QStringLiteral(
			"{\"version\":1,\"type\":\"deviceInfo\",\"computerName\":\"legacy-host\","
			"\"screenWidth\":1366,\"screenHeight\":768,"
			"\"wallpaperMime\":\"image/jpeg\",\"wallpaperData\":\"YWJj\"}");
		KSessionMessage deviceMessage;
		check(KSessionMessageCodec::decode(strLegacyDeviceInfo, &deviceMessage, nullptr),
			QStringLiteral("flat device info JSON decodes"));
		check(deviceMessage.type == DeviceInfoSessionMessageType
			&& deviceMessage.deviceInfo.strComputerName == QStringLiteral("legacy-host")
			&& deviceMessage.deviceInfo.nScreenWidth == 1366,
			QStringLiteral("flat device info fields retain their meaning"));

		KSessionMessage configMessage;
		configMessage.type = StreamConfigSessionMessageType;
		configMessage.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		configMessage.streamConfig.nFps = 30;
		configMessage.streamConfig.nWidth = 1280;
		configMessage.streamConfig.nHeight = 720;
		configMessage.streamConfig.nBitrateKbps = 3000;
		const QJsonObject configObject = QJsonDocument::fromJson(
			KSessionMessageCodec::encode(configMessage).toUtf8()).object();
		check(configObject.value(QStringLiteral("type")).toString() == QStringLiteral("streamConfig")
			&& configObject.value(QStringLiteral("fps")).toInt() == 30
			&& configObject.value(QStringLiteral("width")).toInt() == 1280
			&& configObject.value(QStringLiteral("height")).toInt() == 720
			&& configObject.value(QStringLiteral("bitrateKbps")).toInt() == 3000,
			QStringLiteral("encoded stream config keeps legacy field names and types"));
	}

	void testLanDiscoveryRoundTrip()
	{
		KLanDiscoveryMessage probe;
		probe.type = ProbeLanDiscoveryMessageType;
		probe.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		KLanDiscoveryMessage decodedProbe;
		check(KLanDiscoveryMessageCodec::decode(
			KLanDiscoveryMessageCodec::encode(probe), &decodedProbe, nullptr),
			QStringLiteral("LAN discovery probe decodes"));
		check(decodedProbe.type == ProbeLanDiscoveryMessageType
			&& decodedProbe.strRequestId == probe.strRequestId,
			QStringLiteral("LAN discovery probe round-trips"));

		KLanDiscoveryMessage announcement;
		announcement.type = AnnounceLanDiscoveryMessageType;
		announcement.strRequestId = probe.strRequestId;
		announcement.strInstanceId = QStringLiteral("abcdefab-1234-5678-9abc-def012345678");
		announcement.strDeviceName = QStringLiteral("test-host");
		announcement.nSignalingPort = 39000;
		KLanDiscoveryMessage decodedAnnouncement;
		check(KLanDiscoveryMessageCodec::decode(
			KLanDiscoveryMessageCodec::encode(announcement), &decodedAnnouncement, nullptr),
			QStringLiteral("LAN discovery announcement decodes"));
		check(decodedAnnouncement.strInstanceId == announcement.strInstanceId
			&& decodedAnnouncement.strDeviceName == announcement.strDeviceName
			&& decodedAnnouncement.nSignalingPort == 39000,
			QStringLiteral("LAN discovery announcement round-trips"));
	}

	void testInvalidLanDiscoveryMessages()
	{
		KLanDiscoveryMessage message;
		check(!KLanDiscoveryMessageCodec::decode(QByteArray(2049, 'x'), &message, nullptr),
			QStringLiteral("oversized discovery datagram is rejected"));
		check(!KLanDiscoveryMessageCodec::decode(
			QByteArrayLiteral("{\"protocol\":\"other\",\"version\":1,\"type\":\"probe\","
				"\"requestId\":\"12345678-1234-1234-1234-1234567890ab\"}"),
			&message, nullptr),
			QStringLiteral("foreign discovery protocol is rejected"));
		check(!KLanDiscoveryMessageCodec::decode(
			QByteArrayLiteral("{\"protocol\":\"wrc-lan-discovery\",\"version\":2,"
				"\"type\":\"probe\",\"requestId\":\"bad\"}"),
			&message, nullptr),
			QStringLiteral("unknown discovery version and UUID are rejected"));

		KLanDiscoveryMessage invalidAnnouncement;
		invalidAnnouncement.type = AnnounceLanDiscoveryMessageType;
		invalidAnnouncement.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		invalidAnnouncement.strInstanceId = QStringLiteral("abcdefab-1234-5678-9abc-def012345678");
		invalidAnnouncement.strDeviceName = QString(65, QLatin1Char('a'));
		invalidAnnouncement.nSignalingPort = 0;
		check(!KLanDiscoveryMessageCodec::decode(
			KLanDiscoveryMessageCodec::encode(invalidAnnouncement), &message, nullptr),
			QStringLiteral("invalid port and overlong device name are rejected"));
	}

	void testAccessMessages()
	{
		const QString strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		KAccessMessage request;
		request.type = RequestAccessMessageType;
		request.strRequestId = strRequestId;
		request.strDeviceName = QStringLiteral("controller-host");
		KAccessMessage decoded;
		check(KAccessMessageCodec::decode(KAccessMessageCodec::encode(request), &decoded, nullptr)
			&& decoded.type == RequestAccessMessageType
			&& decoded.strRequestId == strRequestId
			&& decoded.strDeviceName == request.strDeviceName,
			QStringLiteral("access request round-trips"));

		KAccessMessage pending;
		pending.type = PendingAccessMessageType;
		pending.strRequestId = strRequestId;
		pending.nTimeoutSeconds = 30;
		check(KAccessMessageCodec::decode(KAccessMessageCodec::encode(pending), &decoded, nullptr)
			&& decoded.type == PendingAccessMessageType
			&& decoded.nTimeoutSeconds == 30,
			QStringLiteral("access pending round-trips"));

		KAccessMessage accepted;
		accepted.type = AcceptedAccessMessageType;
		accepted.strRequestId = strRequestId;
		check(KAccessMessageCodec::decode(KAccessMessageCodec::encode(accepted), &decoded, nullptr)
			&& decoded.type == AcceptedAccessMessageType,
			QStringLiteral("access accepted round-trips"));

		KAccessMessage rejected;
		rejected.type = RejectedAccessMessageType;
		rejected.strRequestId = strRequestId;
		rejected.strReason = QStringLiteral("user_rejected");
		check(KAccessMessageCodec::decode(KAccessMessageCodec::encode(rejected), &decoded, nullptr)
			&& decoded.type == RejectedAccessMessageType
			&& decoded.strReason == rejected.strReason,
			QStringLiteral("access rejected round-trips"));

		KAccessMessage busy;
		busy.type = ServerBusyAccessMessageType;
		check(KAccessMessageCodec::decode(KAccessMessageCodec::encode(busy), &decoded, nullptr)
			&& decoded.type == ServerBusyAccessMessageType
			&& decoded.strRequestId.isEmpty(),
			QStringLiteral("server busy uses the typed signaling envelope"));
	}

	void testInvalidAccessMessages()
	{
		KAccessMessage message;
		check(!KAccessMessageCodec::decode(
			QStringLiteral("{\"type\":\"accessRequest\",\"version\":2,\"requestId\":\"bad\"}"),
			&message, nullptr),
			QStringLiteral("invalid access version and UUID are rejected"));
		check(!KAccessMessageCodec::decode(
			QStringLiteral("{\"type\":\"accessRequest\",\"version\":1.5,"
				"\"requestId\":\"12345678-1234-1234-1234-1234567890ab\","
				"\"deviceName\":\"host\"}"),
			&message, nullptr),
			QStringLiteral("fractional access version is rejected"));
		check(!KAccessMessageCodec::decode(QString(KAccessMessageCodec::kMaximumMessageBytes + 1,
			QLatin1Char('x')), &message, nullptr),
			QStringLiteral("oversized access message is rejected"));
		check(!KAccessMessageCodec::decode(
			QStringLiteral("{\"type\":\"accessRejected\",\"version\":1,"
				"\"requestId\":\"12345678-1234-1234-1234-1234567890ab\","
				"\"reason\":\"unknown\"}"),
			&message, nullptr),
			QStringLiteral("unknown access rejection reason is rejected"));
		const QString strOverlongName = QStringLiteral(
			"{\"type\":\"accessRequest\",\"version\":1,"
			"\"requestId\":\"12345678-1234-1234-1234-1234567890ab\","
			"\"deviceName\":\"%1\"}").arg(QString(129, QLatin1Char('a')));
		check(!KAccessMessageCodec::decode(strOverlongName, &message, nullptr),
			QStringLiteral("overlong access device name is rejected"));
	}

	void testClipboardMessages()
	{
		const QString strMessageId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		KClipboardMessage source;
		source.type = TextClipboardMessageType;
		source.strMessageId = strMessageId;
		source.strText = QString::fromUtf8("中文\nemoji: 😀");
		KClipboardMessage decoded;
		check(KClipboardMessageCodec::decode(
			KClipboardMessageCodec::encode(source), &decoded, nullptr)
			&& decoded.type == TextClipboardMessageType
			&& decoded.strMessageId == strMessageId
			&& decoded.strText == source.strText,
			QStringLiteral("clipboard text round-trips"));

		source.strText = QString(KClipboardMessageCodec::kMaximumTextBytes, QLatin1Char('a'));
		check(KClipboardMessageCodec::decode(
			KClipboardMessageCodec::encode(source), &decoded, nullptr),
			QStringLiteral("clipboard text size boundary is accepted"));

		KClipboardMessage stateSource;
		stateSource.type = SyncStateClipboardMessageType;
		stateSource.strMessageId = strMessageId;
		stateSource.bEnabled = false;
		check(KClipboardMessageCodec::decode(
			KClipboardMessageCodec::encode(stateSource), &decoded, nullptr)
			&& decoded.type == SyncStateClipboardMessageType
			&& !decoded.bEnabled,
			QStringLiteral("clipboard sync state round-trips"));

		KClipboardMessage readySource;
		readySource.type = ReadyClipboardMessageType;
		readySource.strMessageId = strMessageId;
		check(KClipboardMessageCodec::decode(
			KClipboardMessageCodec::encode(readySource), &decoded, nullptr)
			&& decoded.type == ReadyClipboardMessageType,
			QStringLiteral("clipboard capability handshake round-trips"));
	}

	void testInvalidClipboardMessages()
	{
		KClipboardMessage message;
		check(!KClipboardMessageCodec::decode(QStringLiteral("[]"), &message, nullptr),
			QStringLiteral("non-object clipboard message is rejected"));
		check(!KClipboardMessageCodec::decode(
			QStringLiteral("{\"type\":\"clipboardText\",\"messageId\":\"bad\",\"text\":\"x\"}"),
			&message, nullptr),
			QStringLiteral("invalid clipboard UUID is rejected"));
		check(!KClipboardMessageCodec::decode(
			QStringLiteral("{\"type\":\"clipboardText\",\"messageId\":"
				"\"12345678-1234-1234-1234-1234567890ab\",\"text\":1}"),
			&message, nullptr),
			QStringLiteral("non-string clipboard text is rejected"));

		KClipboardMessage oversized;
		oversized.type = TextClipboardMessageType;
		oversized.strMessageId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		oversized.strText = QString(KClipboardMessageCodec::kMaximumTextBytes + 1, QLatin1Char('a'));
		check(!KClipboardMessageCodec::decode(
			KClipboardMessageCodec::encode(oversized), &message, nullptr),
			QStringLiteral("oversized clipboard text is rejected"));
		check(!KClipboardMessageCodec::decode(
			QStringLiteral("{\"version\":2,\"type\":\"clipboardReady\","
				"\"messageId\":\"12345678-1234-1234-1234-1234567890ab\"}"),
			&message, nullptr),
			QStringLiteral("unsupported clipboard version is rejected"));
		check(!KClipboardMessageCodec::decode(
			QString(KProtocolConstraints::kMaximumClipboardMessageBytes + 1, QLatin1Char('x')),
			&message, nullptr),
			QStringLiteral("oversized clipboard message is rejected"));
	}

	void testProtocolVersionsAndUnknownFields()
	{
		KInputMessage input;
		input.type = MouseMoveInputMessageType;
		const QJsonObject inputObject = QJsonDocument::fromJson(
			KInputMessageCodec::encode(input).toUtf8()).object();
		check(inputObject.value(QStringLiteral("version")).toInt()
			== KProtocolConstraints::kEnvelopeSchemaVersion,
			QStringLiteral("input encoder writes protocol version"));

		KSessionMessage session;
		session.type = StartStreamingSessionMessageType;
		const QJsonObject sessionObject = QJsonDocument::fromJson(
			KSessionMessageCodec::encode(session).toUtf8()).object();
		check(sessionObject.value(QStringLiteral("version")).toInt()
			== KProtocolConstraints::kEnvelopeSchemaVersion,
			QStringLiteral("session encoder writes protocol version"));

		KClipboardMessage clipboard;
		clipboard.type = ReadyClipboardMessageType;
		clipboard.strMessageId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		const QJsonObject clipboardObject = QJsonDocument::fromJson(
			KClipboardMessageCodec::encode(clipboard).toUtf8()).object();
		check(clipboardObject.value(QStringLiteral("version")).toInt()
			== KProtocolConstraints::kEnvelopeSchemaVersion,
			QStringLiteral("clipboard encoder writes protocol version"));

		KInputMessage decoded;
		check(KInputMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"mouseMove\",\"x\":1,\"y\":2,"
				"\"futureField\":true}"), &decoded, nullptr),
			QStringLiteral("unknown fields are ignored for forward compatibility"));
	}

	void testWebRtcSignalingMessages()
	{
		KWebRtcSignalingMessage offer;
		offer.type = OfferWebRtcSignalingMessageType;
		offer.strSdp = QStringLiteral("v=0\\r\\n");
		KWebRtcSignalingMessage decoded;
		check(KWebRtcSignalingMessageCodec::decode(
			KWebRtcSignalingMessageCodec::encode(offer), &decoded, nullptr)
			&& decoded.type == OfferWebRtcSignalingMessageType
			&& decoded.strSdp == offer.strSdp,
			QStringLiteral("WebRTC offer round-trips"));

		KWebRtcSignalingMessage candidate;
		candidate.type = IceCandidateWebRtcSignalingMessageType;
		candidate.strSdpMid = QStringLiteral("0");
		candidate.nSdpMLineIndex = 0;
		candidate.strCandidate = QStringLiteral("candidate:1 1 UDP 1 127.0.0.1 9 typ host");
		check(KWebRtcSignalingMessageCodec::decode(
			KWebRtcSignalingMessageCodec::encode(candidate), &decoded, nullptr)
			&& decoded.type == IceCandidateWebRtcSignalingMessageType,
			QStringLiteral("WebRTC ICE candidate round-trips"));

		check(!KWebRtcSignalingMessageCodec::decode(
			QStringLiteral("{\"version\":2,\"messageType\":\"offer\","
				"\"sdpType\":\"offer\",\"sdp\":\"v=0\"}"), &decoded, nullptr),
			QStringLiteral("unsupported signaling version is rejected"));
		check(!KWebRtcSignalingMessageCodec::decode(
			QString(KProtocolConstraints::kMaximumSignalingMessageBytes + 1, QLatin1Char('x')),
			&decoded, nullptr),
			QStringLiteral("oversized signaling message is rejected"));
	}

	void testSessionCapabilities()
	{
		KSessionMessage source;
		source.type = CapabilitiesSessionMessageType;
		source.capabilities.supportedCodecs = { QStringLiteral("h264") };
		source.capabilities.supportedChannels = {
			QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input"),
			QStringLiteral("input-realtime"), QStringLiteral("clipboard")
		};
		source.capabilities.bInputRealtime = true;
		source.capabilities.supportedPrivacyModes = {
			QStringLiteral("disabled"), QStringLiteral("privacyoverlay")
		};
		source.capabilities.bPostSessionLock = true;
		KMonitorCapability monitor;
		monitor.strId = QStringLiteral("default");
		monitor.nWidth = 1920;
		monitor.nHeight = 1080;
		monitor.bPrimary = true;
		source.capabilities.monitorList.append(monitor);

		KSessionMessage decoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(source),
				&decoded, nullptr)
			&& decoded.type == CapabilitiesSessionMessageType
			&& decoded.capabilities.supportedCodecs.contains(QStringLiteral("h264"))
			&& decoded.capabilities.supportedPrivacyModes.contains(
				QStringLiteral("privacyoverlay"))
			&& decoded.capabilities.bPostSessionLock
			&& decoded.capabilities.monitorList.size() == 1,
			QStringLiteral("session capabilities round-trip"));

		check(!KSessionMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"capabilities\","
				"\"protocolMinVersion\":3,\"protocolMaxVersion\":2}"),
			&decoded, nullptr),
			QStringLiteral("invalid capability version range is rejected"));
	}

	void testTerminalMessages()
	{
		KTerminalMessage source;
		source.type = OpenRequestTerminalMessageType;
		source.strRequestId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
		source.strCommandId = QStringLiteral("6ba7b810-9dad-41d1-80b4-00c04fd430c8");
		source.nColumns = 120;
		source.nRows = 40;
		KTerminalMessage decoded;
		check(KTerminalMessageCodec::decode(
				KTerminalMessageCodec::encode(source), &decoded, nullptr)
			&& decoded.type == OpenRequestTerminalMessageType
			&& decoded.nColumns == 120 && decoded.nRows == 40,
			QStringLiteral("terminal open request round-trips"));

		source.type = ErrorTerminalMessageType;
		source.strErrorCode = QStringLiteral("output_overflow");
		check(KTerminalMessageCodec::decode(
				KTerminalMessageCodec::encode(source), &decoded, nullptr)
			&& decoded.strErrorCode == source.strErrorCode,
			QStringLiteral("terminal error round-trips"));

		check(!KTerminalMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"terminalResize\","
				"\"requestId\":\"550e8400-e29b-41d4-a716-446655440000\","
				"\"columns\":401,\"rows\":40}"), &decoded, nullptr),
			QStringLiteral("terminal size outside bounds is rejected"));
		check(!KTerminalMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"channel\":\"session\","
				"\"type\":\"terminalOpenRequest\",\"requestId\":\"not-a-uuid\","
				"\"payload\":{\"columns\":100,\"rows\":30}}"), &decoded, nullptr),
			QStringLiteral("terminal message rejects invalid request id"));
		check(!KTerminalMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"channel\":\"session\","
				"\"type\":\"terminalUnknown\","
				"\"requestId\":\"550e8400-e29b-41d4-a716-446655440000\","
				"\"payload\":{}}"), &decoded, nullptr),
			QStringLiteral("terminal message rejects unknown control type"));
	}

	void testTerminalDataFrames()
	{
		KTerminalDataFrame source;
		source.direction = InputTerminalDataDirection;
		source.strRequestId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
		source.nSequence = 42;
		source.payload = QByteArray("Get-ChildItem\r\n");
		const QByteArray encoded = KTerminalDataFrameCodec::encode(source);
		KTerminalDataFrame decoded;
		check(KTerminalDataFrameCodec::decode(encoded, &decoded)
			&& decoded.direction == source.direction
			&& decoded.strRequestId == source.strRequestId
			&& decoded.nSequence == source.nSequence
			&& decoded.payload == source.payload,
			QStringLiteral("terminal data frame round-trips"));

		QByteArray malformed = encoded;
		malformed[4] = 2;
		check(!KTerminalDataFrameCodec::decode(malformed, &decoded),
			QStringLiteral("terminal data rejects unsupported version"));
		check(!KTerminalDataFrameCodec::decode(encoded.left(encoded.size() - 1),
			&decoded), QStringLiteral("terminal data rejects truncated payload"));
		KTerminalDataFrame invalid = source;
		invalid.strRequestId = QStringLiteral("not-a-uuid");
		check(KTerminalDataFrameCodec::encode(invalid).isEmpty(),
			QStringLiteral("terminal data rejects invalid request id"));
		QByteArray nullRequestId = encoded;
		for (int nIndex = 8; nIndex < 24; ++nIndex)
			nullRequestId[nIndex] = 0;
		check(!KTerminalDataFrameCodec::decode(nullRequestId, &decoded),
			QStringLiteral("terminal data rejects null request id"));

		source.payload = QByteArray(KTerminalDataFrameCodec::kMaximumPayloadBytes + 1,
			'x');
		check(KTerminalDataFrameCodec::encode(source).isEmpty(),
			QStringLiteral("terminal data rejects oversized payload"));
	}

	void testTlsPairingMessages()
	{
		KTlsPairingMessage source;
		source.type = HelloTlsPairingMessageType;
		source.strRequestId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
		source.strDeviceId = QStringLiteral("123e4567-e89b-12d3-a456-426614174000");
		source.strDeviceName = QStringLiteral("被控端");
		source.strVerificationMethod = QStringLiteral("tls-exporter-numeric-v1");
		source.strTrustCommitId = QStringLiteral(
			"7ca76045-2ded-4f65-9912-16ff5ee3d0cc");
		source.permissions = KPermissionScopes::fromInt(kAllPermissionScopeBits);
		KTlsPairingMessage decoded;
		QString strError;
		check(KTlsPairingMessageCodec::decode(
			KTlsPairingMessageCodec::encode(source), &decoded, &strError)
			&& decoded.type == source.type
			&& decoded.strDeviceId == source.strDeviceId
			&& decoded.strVerificationMethod == source.strVerificationMethod
			&& decoded.strTrustCommitId == source.strTrustCommitId
			&& decoded.permissions == source.permissions,
			QStringLiteral("TLS pairing hello round-trips"));

		source.type = DecisionTlsPairingMessageType;
		source.bAccepted = true;
		check(KTlsPairingMessageCodec::decode(
			KTlsPairingMessageCodec::encode(source), &decoded, nullptr)
			&& decoded.bAccepted,
			QStringLiteral("TLS pairing decision round-trips"));

		source.type = CommittedTlsPairingMessageType;
		check(KTlsPairingMessageCodec::decode(
			KTlsPairingMessageCodec::encode(source), &decoded, nullptr)
			&& decoded.type == CommittedTlsPairingMessageType,
			QStringLiteral("TLS pairing commit round-trips"));

		check(!KTlsPairingMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"channel\":\"signaling\","
				"\"type\":\"identityProof\",\"requestId\":"
				"\"550e8400-e29b-41d4-a716-446655440000\",\"payload\":{}}"),
			&decoded, nullptr),
			QStringLiteral("removed custom identity messages are rejected"));
		check(!KTlsPairingMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"channel\":\"signaling\","
				"\"type\":\"tlsPairingHello\",\"requestId\":\"not-a-uuid\","
				"\"payload\":{}}"), &decoded, nullptr),
			QStringLiteral("TLS pairing rejects invalid request ids"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testMouseMoveRoundTrip();
	testMouseButtonRoundTrip();
	testMouseWheelRoundTrip();
	testKeyRoundTrip();
	testInvalidInputMessages();
	testExtendedInputRoundTrip();
	testLegacyInputWireCompatibility();
	testSessionControlRoundTrip();
	testPrivacyControlRoundTrip();
	testDeviceInfoRoundTrip();
	testEndSessionAndStreamConfigRoundTrip();
	testInvalidSessionMessages();
	testLegacySessionWireCompatibility();
	testLanDiscoveryRoundTrip();
	testInvalidLanDiscoveryMessages();
	testAccessMessages();
	testInvalidAccessMessages();
	testClipboardMessages();
	testInvalidClipboardMessages();
	testProtocolVersionsAndUnknownFields();
	testWebRtcSignalingMessages();
	testSessionCapabilities();
	testTerminalMessages();
	testTerminalDataFrames();
	testTlsPairingMessages();

	if (g_nFailureCount == 0)
		qInfo() << "All protocol codec tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
