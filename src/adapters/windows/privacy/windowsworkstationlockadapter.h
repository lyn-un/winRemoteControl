#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSWORKSTATIONLOCKADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSWORKSTATIONLOCKADAPTER_H_

#include "core/privacy/workstationlockadapter.h"

class KWindowsWorkstationLockAdapter : public IKWorkstationLockAdapter
{
public:
	bool isSupported() const override;
	KPrivacyOperationResult lock() override;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSWORKSTATIONLOCKADAPTER_H_
