#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSDISPLAYPOWERADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSDISPLAYPOWERADAPTER_H_

#include "core/privacy/displaypoweradapter.h"

class KWindowsDisplayPowerAdapter : public IKDisplayPowerAdapter
{
public:
	bool isSupported() const override;
	KPrivacyOperationResult turnOff() override;
	KPrivacyOperationResult restore() override;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSDISPLAYPOWERADAPTER_H_
