#ifndef _WINREMOTECONTROL_SESSION_PAIRINGCOMMAND_H_
#define _WINREMOTECONTROL_SESSION_PAIRINGCOMMAND_H_

#include "core/protocol/tlspairingmessage.h"
#include "session/pairingtransaction.h"

#include <QtCore/QHash>

#include <optional>

enum KPairingHelloDisposition
{
	NewPairingHelloDisposition,
	DuplicatePairingHelloDisposition,
	ConflictingRequestPairingHelloDisposition,
	ConflictingPayloadPairingHelloDisposition
};

class KPairingCommand final
{
public:
	void begin(const QString &strRequestId, quint64 nGeneration);
	void reset();

	KPairingHelloDisposition classifyHello(
		const KTlsPairingMessage &message,
		quint64 nGeneration) const;
	void acceptHello(const KTlsPairingMessage &message);
	void cacheResponse(const KTlsPairingMessage &message);
	std::optional<KTlsPairingMessage> replayResponse() const;

	KPairingTransaction &transaction();
	const KPairingTransaction &transaction() const;

private:
	KPairingTransaction m_transaction;
	KTlsPairingMessage m_remoteHello;
	QHash<int, KTlsPairingMessage> m_cachedResponses;
};

#endif // _WINREMOTECONTROL_SESSION_PAIRINGCOMMAND_H_
