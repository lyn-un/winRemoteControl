#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_ACCESSMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_ACCESSMESSAGE_H_

#include "core/protocol/protocolconstraints.h"

#include <QtCore/QString>

struct KProtocolEnvelope;

enum KAccessMessageType
{
	InvalidAccessMessageType,
	RequestAccessMessageType,
	PendingAccessMessageType,
	AcceptedAccessMessageType,
	RejectedAccessMessageType
};

struct KAccessMessage
{
	KAccessMessageType type = InvalidAccessMessageType;
	QString strRequestId;
	QString strDeviceName;
	QString strReason;
	int nTimeoutSeconds = 0;
};

class KAccessMessageCodec
{
public:
	static constexpr int kMaximumMessageBytes = KProtocolConstraints::kMaximumAccessMessageBytes;

	static QString encode(const KAccessMessage &message);
	static bool decode(const QString &strMessage,
		KAccessMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KAccessMessage *pMessage,
		QString *pErrorMessage);
	static bool isAccessMessage(const QString &strMessage);
	static QString typeName(KAccessMessageType type);
	static bool isValidRejectReason(const QString &strReason);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_ACCESSMESSAGE_H_
