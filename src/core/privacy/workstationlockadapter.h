#ifndef _WINREMOTECONTROL_CORE_PRIVACY_WORKSTATIONLOCKADAPTER_H_
#define _WINREMOTECONTROL_CORE_PRIVACY_WORKSTATIONLOCKADAPTER_H_

#include "core/privacy/privacytypes.h"

class IKWorkstationLockAdapter
{
public:
	virtual ~IKWorkstationLockAdapter() = default;
	virtual bool isSupported() const = 0;
	virtual KPrivacyOperationResult lock() = 0;
};

#endif // _WINREMOTECONTROL_CORE_PRIVACY_WORKSTATIONLOCKADAPTER_H_
