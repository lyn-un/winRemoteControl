#include "core/protocol/inputmessage.h"
#include "core/protocol/landiscoverymessage.h"
#include "core/protocol/sessionmessage.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;

		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
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
		source.nSequence = 99;

		KInputMessage decoded;
		check(KInputMessageCodec::decode(KInputMessageCodec::encode(source), &decoded, nullptr),
			QStringLiteral("key input decodes"));
		check(decoded.nVirtualKey == source.nVirtualKey
			&& !decoded.bPressed
			&& decoded.bExtended,
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
			QStringLiteral("{\"type\":\"mouseButton\",\"button\":\"middle\",\"pressed\":true,\"x\":1,\"y\":2}"),
			&message,
			nullptr),
			QStringLiteral("unknown mouse button is rejected"));
	}

	void testLegacyInputWireCompatibility()
	{
		const QString strLegacyMouse = QStringLiteral(
			"{\"type\":\"mouseButton\",\"button\":\"left\",\"pressed\":true,"
			"\"x\":11,\"y\":22,\"seq\":\"7\",\"trace\":true}");
		KInputMessage mouseMessage;
		check(KInputMessageCodec::decode(strLegacyMouse, &mouseMessage, nullptr),
			QStringLiteral("legacy mouse JSON decodes"));
		check(mouseMessage.type == MouseButtonInputMessageType
			&& mouseMessage.mouseButton == LeftRemoteMouseButton
			&& mouseMessage.nSequence == 7,
			QStringLiteral("legacy mouse fields retain their meaning"));

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
			KSessionMessage decoded;
			check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(source), &decoded, nullptr),
				QStringLiteral("session control message decodes"));
			check(decoded.type == type, QStringLiteral("session control type round-trips"));
		}
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
		endSource.strReason = QStringLiteral("test_stop");
		KSessionMessage endDecoded;
		check(KSessionMessageCodec::decode(KSessionMessageCodec::encode(endSource), &endDecoded, nullptr),
			QStringLiteral("end session decodes"));
		check(endDecoded.strReason == endSource.strReason,
			QStringLiteral("end session reason round-trips"));

		KSessionMessage configSource;
		configSource.type = StreamConfigSessionMessageType;
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
	}

	void testLegacySessionWireCompatibility()
	{
		const QString strLegacyDeviceInfo = QStringLiteral(
			"{\"type\":\"deviceInfo\",\"computerName\":\"legacy-host\","
			"\"screenWidth\":1366,\"screenHeight\":768,"
			"\"wallpaperMime\":\"image/jpeg\",\"wallpaperData\":\"YWJj\"}");
		KSessionMessage deviceMessage;
		check(KSessionMessageCodec::decode(strLegacyDeviceInfo, &deviceMessage, nullptr),
			QStringLiteral("legacy device info JSON decodes"));
		check(deviceMessage.type == DeviceInfoSessionMessageType
			&& deviceMessage.deviceInfo.strComputerName == QStringLiteral("legacy-host")
			&& deviceMessage.deviceInfo.nScreenWidth == 1366,
			QStringLiteral("legacy device info fields retain their meaning"));

		KSessionMessage configMessage;
		configMessage.type = StreamConfigSessionMessageType;
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
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testMouseMoveRoundTrip();
	testMouseButtonRoundTrip();
	testMouseWheelRoundTrip();
	testKeyRoundTrip();
	testInvalidInputMessages();
	testLegacyInputWireCompatibility();
	testSessionControlRoundTrip();
	testDeviceInfoRoundTrip();
	testEndSessionAndStreamConfigRoundTrip();
	testInvalidSessionMessages();
	testLegacySessionWireCompatibility();
	testLanDiscoveryRoundTrip();
	testInvalidLanDiscoveryMessages();

	if (g_nFailureCount == 0)
		qInfo() << "All protocol codec tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
