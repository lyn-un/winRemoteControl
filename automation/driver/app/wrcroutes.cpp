#include "automation/driver/app/wrcroutes.h"

bool RegisterWrcRoutes(KRequestRouter *pRouter, QString *pErrorMessage)
{
	if (pRouter == nullptr)
		return false;
	const KDriverRouteHandler handler = [](const KParsedDriverRequest &,
		const QHash<QString, QString> &) {};
	return pRouter->registerRoute(QStringLiteral("GET"),
			QStringLiteral("/status"), handler, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("POST"),
			QStringLiteral("/session"), handler, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("DELETE"),
			QStringLiteral("/session/:sessionId"), handler, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("POST"),
			QStringLiteral("/session/:sessionId/command/trigger"), handler, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("GET"),
			QStringLiteral("/session/:sessionId/state"), handler, pErrorMessage)
		&& pRouter->registerRoute(QStringLiteral("GET"),
			QStringLiteral("/session/:sessionId/events"), handler, pErrorMessage);
}
