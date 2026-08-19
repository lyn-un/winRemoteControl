#ifndef _WINREMOTECONTROL_CORE_PRIVACY_PRIVACYOVERLAYADAPTER_H_
#define _WINREMOTECONTROL_CORE_PRIVACY_PRIVACYOVERLAYADAPTER_H_

#include "core/privacy/privacytypes.h"

#include <functional>

class IKPrivacyOverlayAdapter
{
public:
	using EmergencyRestoreHandler = std::function<void()>;

	virtual ~IKPrivacyOverlayAdapter() = default;
	virtual bool isSupported() const = 0;
	virtual KPrivacyOperationResult apply() = 0;
	virtual KPrivacyOperationResult restore() = 0;
	virtual void setEmergencyRestoreHandler(EmergencyRestoreHandler handler) = 0;
};

#endif // _WINREMOTECONTROL_CORE_PRIVACY_PRIVACYOVERLAYADAPTER_H_
