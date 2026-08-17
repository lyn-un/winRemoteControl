#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_TLSPAIRINGMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_TLSPAIRINGMESSAGE_H_

#include "core/protocol/protocolconstraints.h"
#include "core/security/permissionscope.h"

#include <QtCore/QString>

struct KProtocolEnvelope;

enum KTlsPairingMessageType
{
	InvalidTlsPairingMessageType,
	HelloTlsPairingMessageType,
	DecisionTlsPairingMessageType,
	ReadyTlsPairingMessageType,
	CommittedTlsPairingMessageType,
	RejectedTlsPairingMessageType
};

struct KTlsPairingMessage
{
	KTlsPairingMessageType type = InvalidTlsPairingMessageType;
	QString strRequestId;
	QString strDeviceId;
	QString strDeviceName;
	QString strVerificationMethod;
	QString strTrustCommitId;
	KPermissionScopes permissions;
	bool bAccepted = false;
	QString strReason;
};

// These are TLS-protected pairing control messages. They contain no keys,
// nonces, proofs, pairing secrets, or signatures; Schannel authenticates the
// peer certificate before this codec is reachable.
class KTlsPairingMessageCodec
{
public:
	static constexpr int kMaximumMessageBytes = KProtocolConstraints::kMaximumAccessMessageBytes;

	static QString encode(const KTlsPairingMessage &message);
	static bool decode(const QString &strMessage,
		KTlsPairingMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KTlsPairingMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KTlsPairingMessageType type);
	static bool isTlsPairingType(const QString &strType);
	static bool isValidRejectReason(const QString &strReason);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_TLSPAIRINGMESSAGE_H_
