#ifndef _WINREMOTECONTROL_CORE_SESSION_CAPABILITYNEGOTIATOR_H_
#define _WINREMOTECONTROL_CORE_SESSION_CAPABILITYNEGOTIATOR_H_

#include "core/protocol/sessionmessage.h"

enum KCapabilityNegotiationStatus
{
	CapabilityNegotiationSucceeded,
	IncompatibleVersionCapabilityNegotiationStatus,
	MissingCodecCapabilityNegotiationStatus,
	MissingChannelCapabilityNegotiationStatus
};

struct KCapabilityNegotiationResult
{
	KCapabilityNegotiationStatus status = MissingChannelCapabilityNegotiationStatus;
	KNegotiatedCapabilities capabilities;
	QString strTechnicalMessage;

	bool succeeded() const;
};

class KCapabilityNegotiator
{
public:
	static KCapabilityNegotiationResult negotiate(
		const KSessionCapabilities &localCapabilities,
		const KSessionCapabilities &remoteCapabilities);
};

#endif // _WINREMOTECONTROL_CORE_SESSION_CAPABILITYNEGOTIATOR_H_
