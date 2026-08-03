#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLENVELOPE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLENVELOPE_H_

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

enum KProtocolChannel
{
	InvalidProtocolChannel,
	SignalingProtocolChannel,
	InputProtocolChannel,
	SessionProtocolChannel,
	ClipboardProtocolChannel
};

struct KProtocolEnvelope
{
	int nVersion = 0;
	KProtocolChannel channel = InvalidProtocolChannel;
	QString strType;
	QString strRequestId;
	quint64 nSequence = 0;
	QJsonObject payload;
	QString strRawMessage;
	int nEncodedBytes = 0;
};

class KProtocolEnvelopeCodec
{
public:
	static bool decode(KProtocolChannel channel,
		const QString &strMessage,
		KProtocolEnvelope *pEnvelope,
		QString *pErrorMessage);
	static QString channelName(KProtocolChannel channel);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLENVELOPE_H_
