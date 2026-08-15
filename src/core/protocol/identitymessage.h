#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_IDENTITYMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_IDENTITYMESSAGE_H_

#include "core/protocol/protocolconstraints.h"
#include "core/security/permissionscope.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

struct KProtocolEnvelope;

enum KIdentityMessageType
{
	InvalidIdentityMessageType,
	HelloIdentityMessageType,
	ChallengeIdentityMessageType,
	ProofIdentityMessageType,
	PairingDecisionIdentityMessageType,
	AuthenticatedIdentityMessageType,
	RejectedIdentityMessageType
};

struct KIdentityMessage
{
	KIdentityMessageType type = InvalidIdentityMessageType;
	QString strRequestId;
	QString strDeviceId;
	QString strDeviceName;
	QByteArray publicKey;
	QByteArray nonce;
	QByteArray signature;
	QByteArray transcriptHash;
	KPermissionScopes permissions;
	bool bAccepted = false;
	QString strReason;
};

class KIdentityMessageCodec
{
public:
	static constexpr int kMaximumMessageBytes = KProtocolConstraints::kMaximumAccessMessageBytes;

	static QString encode(const KIdentityMessage &message);
	static bool decode(const QString &strMessage,
		KIdentityMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KIdentityMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KIdentityMessageType type);
	static bool isIdentityType(const QString &strType);
	static bool isValidRejectReason(const QString &strReason);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_IDENTITYMESSAGE_H_
