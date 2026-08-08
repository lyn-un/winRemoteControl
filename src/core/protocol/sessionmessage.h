#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_

#include "core/media/streamconfig.h"
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
	CapabilityRejectedSessionMessageType
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
	bool bKeyboard = true;
	bool bUnicodeText = true;
	bool bMouseButtons = true;
	bool bMouseWheel = true;
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
	bool bKeyboard = false;
	bool bUnicodeText = false;
	bool bMouseButtons = false;
	bool bMouseWheel = false;
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
	QString strReason;
	KRemoteDeviceInfo deviceInfo;
	KStreamConfig streamConfig;
	KSessionCapabilities capabilities;
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
	static bool negotiate(const KSessionCapabilities &local,
		const KSessionCapabilities &remote,
		KNegotiatedCapabilities *pNegotiated,
		QString *pErrorMessage);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_
