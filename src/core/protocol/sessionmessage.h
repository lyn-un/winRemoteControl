#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_

#include "core/media/streamconfig.h"

#include <QtCore/QString>

enum KSessionMessageType
{
	InvalidSessionMessageType,
	DeviceInfoRequestSessionMessageType,
	DeviceInfoSessionMessageType,
	StartStreamingSessionMessageType,
	StopStreamingSessionMessageType,
	EndSessionMessageType,
	StreamConfigSessionMessageType
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
};

class KSessionMessageCodec
{
public:
	static constexpr int kProtocolVersion = 1;

	static QString encode(const KSessionMessage &message);
	static bool decode(const QString &strMessage, KSessionMessage *pMessage, QString *pErrorMessage);
	static QString typeName(KSessionMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_SESSIONMESSAGE_H_
