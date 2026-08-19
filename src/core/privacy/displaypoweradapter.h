#ifndef _WINREMOTECONTROL_CORE_PRIVACY_DISPLAYPOWERADAPTER_H_
#define _WINREMOTECONTROL_CORE_PRIVACY_DISPLAYPOWERADAPTER_H_

#include "core/privacy/privacytypes.h"

class IKDisplayPowerAdapter
{
public:
	virtual ~IKDisplayPowerAdapter() = default;
	virtual bool isSupported() const = 0;
	virtual KPrivacyOperationResult turnOff() = 0;
	virtual KPrivacyOperationResult restore() = 0;
};

#endif // _WINREMOTECONTROL_CORE_PRIVACY_DISPLAYPOWERADAPTER_H_
