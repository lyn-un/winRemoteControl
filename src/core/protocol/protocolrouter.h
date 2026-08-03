#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLROUTER_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLROUTER_H_

#include "core/protocol/protocolenvelope.h"

#include <QtCore/QHash>

#include <functional>

struct KProtocolRouteContext
{
	int nRole = 0;
	int nState = 0;
	quint64 nGeneration = 0;
};

enum KProtocolRouteStatus
{
	HandledProtocolRouteStatus,
	MalformedProtocolRouteStatus,
	UnsupportedVersionProtocolRouteStatus,
	UnknownTypeProtocolRouteStatus,
	ForbiddenProtocolRouteStatus
};

struct KProtocolRouteResult
{
	KProtocolRouteStatus status = MalformedProtocolRouteStatus;
	KProtocolEnvelope envelope;
	QString strError;
};

class KProtocolRouter
{
public:
	using Guard = std::function<bool(const KProtocolEnvelope &, const KProtocolRouteContext &)>;
	using Handler = std::function<void(const KProtocolEnvelope &)>;

	bool registerHandler(KProtocolChannel channel,
		const QString &strType,
		Guard guard,
		Handler handler);
	KProtocolRouteResult route(KProtocolChannel channel,
		const QString &strMessage,
		const KProtocolRouteContext &context) const;

private:
	struct KRouteEntry
	{
		Guard guard;
		Handler handler;
	};

	static QString routeKey(KProtocolChannel channel, const QString &strType);

	QHash<QString, KRouteEntry> m_routes;
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_PROTOCOLROUTER_H_
