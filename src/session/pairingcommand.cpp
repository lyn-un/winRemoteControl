#include "session/pairingcommand.h"

void KPairingCommand::begin(const QString &strRequestId,
	quint64 nGeneration)
{
	reset();
	m_transaction.begin(strRequestId, nGeneration);
}

void KPairingCommand::reset()
{
	m_transaction.reset();
	m_remoteHello = KTlsPairingMessage();
	m_cachedResponses.clear();
}

KPairingHelloDisposition KPairingCommand::classifyHello(
	const KTlsPairingMessage &message,
	quint64 nGeneration) const
{
	if (!m_transaction.isActive())
		return NewPairingHelloDisposition;
	if (!m_transaction.matches(message.strRequestId, nGeneration))
		return ConflictingRequestPairingHelloDisposition;
	if (!m_transaction.helloReceived())
		return NewPairingHelloDisposition;
	return KTlsPairingMessageCodec::encode(message)
		== KTlsPairingMessageCodec::encode(m_remoteHello)
		? DuplicatePairingHelloDisposition
		: ConflictingPayloadPairingHelloDisposition;
}

void KPairingCommand::acceptHello(const KTlsPairingMessage &message)
{
	m_remoteHello = message;
}

void KPairingCommand::cacheResponse(const KTlsPairingMessage &message)
{
	m_cachedResponses.insert(static_cast<int>(message.type), message);
}

std::optional<KTlsPairingMessage> KPairingCommand::replayResponse() const
{
	for (KTlsPairingMessageType type : { RejectedTlsPairingMessageType,
		CommittedTlsPairingMessageType, ReadyTlsPairingMessageType,
		DecisionTlsPairingMessageType })
	{
		const auto iterator = m_cachedResponses.constFind(static_cast<int>(type));
		if (iterator != m_cachedResponses.constEnd())
			return iterator.value();
	}
	return std::nullopt;
}

KPairingTransaction &KPairingCommand::transaction()
{
	return m_transaction;
}

const KPairingTransaction &KPairingCommand::transaction() const
{
	return m_transaction;
}
