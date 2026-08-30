#ifndef _WINREMOTECONTROL_DRIVER_REQUESTROUTER_H_
#define _WINREMOTECONTROL_DRIVER_REQUESTROUTER_H_

#include "automation/driver/base/requestparser.h"

#include <QtCore/QHash>
#include <QtCore/QList>

#include <functional>

using KDriverRouteHandler = std::function<void(quint64,
	const KParsedDriverRequest &,
	const QHash<QString, QString> &)>;

class KRequestRouter
{
public:
	bool registerRoute(const QString &strMethod,
		const QString &strPathTemplate,
		const KDriverRouteHandler &handler,
		QString *pErrorMessage);
	bool route(quint64 nRequestId, const KParsedDriverRequest &request) const;

private:
	struct KRoute
	{
		QString strMethod;
		QString strPathTemplate;
		QStringList pathSegments;
		KDriverRouteHandler handler;
	};

	bool match(const KRoute &route,
		const KParsedDriverRequest &request,
		QHash<QString, QString> *pPathParameters) const;

	QList<KRoute> m_routes;
};

#endif // _WINREMOTECONTROL_DRIVER_REQUESTROUTER_H_
