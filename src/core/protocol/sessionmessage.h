#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_

#include "core/media/streamconfig.h"
#include "core/privacy/privacytypes.h"
#include "core/protocol/protocolconstraints.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

struct KProtocolEnvelope;

enum KSessionMessageType
{
	InvalidSessionMessageType,
	DeviceInfoRequestSessionMessageType,
	DeviceInfoSessionMessageType,
	StartStreamingSessionMessageType,
	StopStreamingSessionMessageType,
	EndSessionMessageType,
	StreamConfigSessionMessageType,
	CapabilitiesSessionMessageType,
	CapabilityRejectedSessionMessageType,
	SetPrivacyModeSessionMessageType,
	PrivacyModeStateSessionMessageType,
	SetPostSessionActionSessionMessageType,
	PostSessionActionStateSessionMessageType,
	CommandResultSessionMessageType
};

struct KMonitorCapability
{
	QString strId;
	int nWidth = 0;
	int nHeight = 0;
	bool bPrimary = false;
};

struct KSessionCapabilities
{
	int nProtocolMinVersion = KProtocolConstraints::kSessionProtocolMinVersion;
	int nProtocolMaxVersion = KProtocolConstraints::kSessionProtocolMaxVersion;
	QStringList supportedCodecs;
	QStringList supportedChannels;
	int nMaximumWidth = 1920;
	int nMaximumHeight = 1080;
	int nMaximumFps = 60;
	int nMaximumBitrateKbps = 20000;
	bool bClipboardText = true;
	bool bInputRealtime = false;
	bool bKeyboard = true;
	bool bUnicodeText = true;
	bool bMouseButtons = true;
	bool bMouseWheel = true;
	QStringList supportedPrivacyModes;
	bool bPostSessionLock = false;
	QVector<KMonitorCapability> monitorList;
};

struct KNegotiatedCapabilities
{
	bool bValid = false;
	int nProtocolVersion = 0;
	QString strVideoCodec;
	QStringList channels;
	int nMaximumWidth = 0;
	int nMaximumHeight = 0;
	int nMaximumFps = 0;
	int nMaximumBitrateKbps = 0;
	bool bClipboardText = false;
	bool bInputRealtime = false;
	bool bKeyboard = false;
	bool bUnicodeText = false;
	bool bMouseButtons = false;
	bool bMouseWheel = false;
	bool bFileTransfer = false;
	QStringList supportedPrivacyModes;
	bool bPostSessionLock = false;
};

struct KRemoteDeviceInfo
{
	QString strComputerName;
	QString strWallpaperMime;
	QString strWallpaperData;
	int nScreenWidth = 0;
	int nScreenHeight = 0;
};

struct KSessionMessage
{
	KSessionMessageType type = InvalidSessionMessageType;
	QString strRequestId;
	QString strReason;
	QString strErrorCode;
	bool bSuccess = false;
	bool bHasStreamConfig = false;
	KRemoteDeviceInfo deviceInfo;
	KStreamConfig streamConfig;
	KSessionCapabilities capabilities;
	KPrivacyMode privacyMode = DisabledPrivacyMode;
	KPrivacyModeStatus privacyModeStatus;
	KPostSessionAction postSessionAction = NoPostSessionAction;
	KPostSessionActionStatus postSessionActionStatus;
};

class KSessionMessageCodec
{
public:
	static QString encode(const KSessionMessage &message);
	static bool decode(const QString &strMessage, KSessionMessage *pMessage, QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KSessionMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KSessionMessageType type);
	static bool isCommand(KSessionMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_
