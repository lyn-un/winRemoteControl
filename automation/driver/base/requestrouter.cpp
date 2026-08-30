#include "automation/driver/base/requestrouter.h"

namespace
{
	QStringList PathSegments(const QString &strPath)
	{
		return strPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
	}
}

bool KRequestRouter::registerRoute(const QString &strMethod,
	const QString &strPathTemplate,
	const KDriverRouteHandler &handler,
	QString *pErrorMessage)
{
	if (strMethod.trimmed().isEmpty() || !strPathTemplate.startsWith(QLatin1Char('/'))
		|| !handler)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid route");
		return false;
	}
	for (const KRoute &existing : m_routes)
	{
		if (existing.strMethod == strMethod.toUpper()
			&& existing.strPathTemplate == strPathTemplate)
		{
			if (pErrorMessage != nullptr)
				*pErrorMessage = QStringLiteral("Route is already registered");
			return false;
		}
	}
	KRoute route;
	route.strMethod = strMethod.toUpper();
	route.strPathTemplate = strPathTemplate;
	route.pathSegments = PathSegments(strPathTemplate);
	route.handler = handler;
	m_routes.append(route);
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

bool KRequestRouter::route(quint64 nRequestId, const KParsedDriverRequest &request) const
{
	for (const KRoute &route : m_routes)
	{
		QHash<QString, QString> pathParameters;
		if (!match(route, request, &pathParameters))
			continue;
		route.handler(nRequestId, request, pathParameters);
		return true;
	}
	return false;
}

bool KRequestRouter::match(const KRoute &route,
	const KParsedDriverRequest &request,
	QHash<QString, QString> *pPathParameters) const
{
	if (route.strMethod != request.strMethod || pPathParameters == nullptr)
		return false;
	const QString strPath = request.strPath.section(QLatin1Char('?'), 0, 0);
	const QStringList segments = PathSegments(strPath);
	if (segments.size() != route.pathSegments.size())
		return false;
	for (int i = 0; i < segments.size(); ++i)
	{
		const QString strExpected = route.pathSegments.at(i);
		if (strExpected.startsWith(QLatin1Char(':')))
		{
			if (segments.at(i).isEmpty())
				return false;
			pPathParameters->insert(strExpected.mid(1), segments.at(i));
		}
		else if (strExpected != segments.at(i))
		{
			return false;
		}
	}
	return true;
}
