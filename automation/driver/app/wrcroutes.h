#ifndef _WINREMOTECONTROL_DRIVER_WRCROUTES_H_
#define _WINREMOTECONTROL_DRIVER_WRCROUTES_H_

#include "automation/driver/base/requestrouter.h"

bool RegisterWrcRoutes(KRequestRouter *pRouter, QString *pErrorMessage);

#endif // _WINREMOTECONTROL_DRIVER_WRCROUTES_H_
