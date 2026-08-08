#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_WEBRTCSIGNALINGMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_WEBRTCSIGNALINGMESSAGE_H_

#include <QtCore/QString>

struct KProtocolEnvelope;

enum KWebRtcSignalingMessageType
{
	InvalidWebRtcSignalingMessageType,
	OfferWebRtcSignalingMessageType,
	AnswerWebRtcSignalingMessageType,
	IceCandidateWebRtcSignalingMessageType
};

struct KWebRtcSignalingMessage
{
	KWebRtcSignalingMessageType type = InvalidWebRtcSignalingMessageType;
	QString strSdp;
	QString strSdpMid;
	QString strCandidate;
	int nSdpMLineIndex = 0;
};

class KWebRtcSignalingMessageCodec
{
public:
	static QString encode(const KWebRtcSignalingMessage &message);
	static bool decode(const QString &strMessage,
		KWebRtcSignalingMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KWebRtcSignalingMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KWebRtcSignalingMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_WEBRTCSIGNALINGMESSAGE_H_
