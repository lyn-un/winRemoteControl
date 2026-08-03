#ifndef _WINREMOTECONTROL_SESSION_SESSIONERRORPRESENTER_H_
#define _WINREMOTECONTROL_SESSION_SESSIONERRORPRESENTER_H_

#include "core/session/sessionerror.h"

class KSessionErrorPresenter
{
public:
	static QString userMessage(const KSessionError &error);
};

#endif // _WINREMOTECONTROL_SESSION_SESSIONERRORPRESENTER_H_
