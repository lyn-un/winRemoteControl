#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_LANDISCOVERYMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_LANDISCOVERYMESSAGE_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

enum KLanDiscoveryMessageType
{
	InvalidLanDiscoveryMessageType,
	ProbeLanDiscoveryMessageType,
	AnnounceLanDiscoveryMessageType
};

struct KLanDiscoveryMessage
{
	KLanDiscoveryMessageType type = InvalidLanDiscoveryMessageType;
	QString strRequestId;
	QString strInstanceId;
	QString strDeviceName;
	quint16 nSignalingPort = 0;
};

class KLanDiscoveryMessageCodec
{
public:
	static constexpr int kMaximumDatagramSize = 2048;
	static constexpr int kMaximumDeviceNameLength = 64;

	static QByteArray encode(const KLanDiscoveryMessage &message);
	static bool decode(const QByteArray &data,
		KLanDiscoveryMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KLanDiscoveryMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_LANDISCOVERYMESSAGE_H_
