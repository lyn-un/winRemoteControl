#include "core/protocol/protocolrouter.h"

#include "core/protocol/protocolconstraints.h"

bool KProtocolRouter::registerHandler(KProtocolChannel channel,
	const QString &strType,
	Guard guard,
	Handler handler)
{
	if (channel == InvalidProtocolChannel || strType.isEmpty() || !handler)
		return false;
	const QString strKey = routeKey(channel, strType);
	if (m_routes.contains(strKey))
		return false;
	m_routes.insert(strKey, { std::move(guard), std::move(handler) });
	return true;
}

KProtocolRouteResult KProtocolRouter::route(KProtocolChannel channel,
	const QString &strMessage,
	const KProtocolRouteContext &context) const
{
	KProtocolRouteResult result;
	if (!KProtocolEnvelopeCodec::decode(channel, strMessage,
		&result.envelope, &result.strError))
	{
		return result;
	}
	if (result.envelope.nVersion != KProtocolConstraints::kProtocolVersion)
	{
		result.status = UnsupportedVersionProtocolRouteStatus;
		result.strError = QStringLiteral("Unsupported protocol version");
		return result;
	}

	const auto iterator = m_routes.constFind(routeKey(channel, result.envelope.strType));
	if (iterator == m_routes.constEnd())
	{
		result.status = UnknownTypeProtocolRouteStatus;
		result.strError = QStringLiteral("Unknown protocol message type");
		return result;
	}
	if (iterator->guard && !iterator->guard(result.envelope, context))
	{
		result.status = ForbiddenProtocolRouteStatus;
		result.strError = QStringLiteral("Protocol message is not allowed in the current session state");
		return result;
	}

	iterator->handler(result.envelope);
	result.status = HandledProtocolRouteStatus;
	return result;
}

QString KProtocolRouter::routeKey(KProtocolChannel channel, const QString &strType)
{
	return QStringLiteral("%1:%2").arg(static_cast<int>(channel)).arg(strType);
}
