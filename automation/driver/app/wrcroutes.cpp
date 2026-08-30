#include "automation/driver/app/wrcroutes.h"

bool RegisterWrcRoutes(KRequestRouter *pRouter,
	const KWrcRouteHandlers &handlers,
	QString *pErrorMessage)
{
	if (pRouter == nullptr || !handlers.status || !handlers.createSession
		|| !handlers.deleteSession || !handlers.triggerCommand
		|| !handlers.stateSnapshot || !handlers.eventsSnapshot)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Route handlers are incomplete");
		return false;
	}
	return pRouter->registerRoute(QStringLiteral("GET"),
			QStringLiteral("/status"), handlers.status, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("POST"),
			QStringLiteral("/session"), handlers.createSession, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("DELETE"),
			QStringLiteral("/session/:sessionId"), handlers.deleteSession, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("POST"),
			QStringLiteral("/session/:sessionId/command/trigger"),
			handlers.triggerCommand, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("GET"),
			QStringLiteral("/session/:sessionId/state"), handlers.stateSnapshot, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("GET"),
			QStringLiteral("/session/:sessionId/events"), handlers.eventsSnapshot, pErrorMessage);
}
