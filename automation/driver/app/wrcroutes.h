#ifndef _WINREMOTECONTROL_DRIVER_WRCROUTES_H_
#define _WINREMOTECONTROL_DRIVER_WRCROUTES_H_

#include "automation/driver/base/requestrouter.h"

struct KWrcRouteHandlers
{
	KDriverRouteHandler status;
	KDriverRouteHandler createSession;
	KDriverRouteHandler deleteSession;
	KDriverRouteHandler triggerCommand;
	KDriverRouteHandler stateSnapshot;
	KDriverRouteHandler eventsSnapshot;
};

bool RegisterWrcRoutes(KRequestRouter *pRouter,
	const KWrcRouteHandlers &handlers,
	QString *pErrorMessage);

#endif // _WINREMOTECONTROL_DRIVER_WRCROUTES_H_
