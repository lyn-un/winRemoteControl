#ifndef _WINREMOTECONTROL_SESSION_SECURITYSESSIONERRORMAPPER_H_
#define _WINREMOTECONTROL_SESSION_SECURITYSESSIONERRORMAPPER_H_

#include "core/security/securitystatus.h"
#include "core/session/sessionerror.h"

class KSecuritySessionErrorMapper final
{
public:
	static KSessionError map(const KSecurityStatus &status);

private:
	static KSessionErrorCode errorCode(KSecurityErrorCode code);
	static KSessionErrorStage errorStage(KSecurityStage stage);
};

#endif // _WINREMOTECONTROL_SESSION_SECURITYSESSIONERRORMAPPER_H_
