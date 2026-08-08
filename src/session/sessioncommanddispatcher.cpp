#include "session/sessioncommanddispatcher.h"

#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <utility>

namespace
{
	constexpr int kCommandTimeoutMs = 1000;
	constexpr int kTimerIntervalMs = 100;
	constexpr int kMaximumCommandAttempts = 2;
	constexpr int kMaximumRecentCommandResults = 128;
}

KSessionCommandDispatcher::KSessionCommandDispatcher(QObject *pParent)
	: QObject(pParent)
	, m_pTimer(new QTimer(this))
{
	m_pTimer->setInterval(kTimerIntervalMs);
	m_clock.start();
	connect(m_pTimer, &QTimer::timeout,
		this, &KSessionCommandDispatcher::handleTimer);
}

void KSessionCommandDispatcher::setTransmitFunction(TransmitFunction transmitFunction)
{
	m_transmitFunction = std::move(transmitFunction);
}

void KSessionCommandDispatcher::registerHandler(KSessionMessageType type,
	Handler handler)
{
	m_handlers.insert(static_cast<int>(type), std::move(handler));
}

QString KSessionCommandDispatcher::send(KSessionMessage message, quint64 nGeneration)
{
	if (KSessionMessageCodec::isCommand(message.type) && message.strRequestId.isEmpty())
		message.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const KSessionCommandTransmitResult transmitResult = transmit(message);
	if (!transmitResult.bAccepted)
	{
		if (KSessionMessageCodec::isCommand(message.type))
		{
			emit commandCompleted(message.type,
				message.strRequestId,
				false,
				transmitResult.strErrorCode.isEmpty()
					? QStringLiteral("send_failed") : transmitResult.strErrorCode,
				nGeneration);
		}
		return QString();
	}
	if (!KSessionMessageCodec::isCommand(message.type))
		return message.strRequestId;

	KPendingCommand pending;
	pending.message = message;
	pending.nSentMs = m_clock.elapsed();
	pending.nAttempts = 1;
	pending.nGeneration = nGeneration;
	m_pendingCommands.insert(message.strRequestId, pending);
	if (!m_pTimer->isActive())
		m_pTimer->start();
	return message.strRequestId;
}

KSessionIncomingDispatchResult KSessionCommandDispatcher::handleIncoming(
	const KSessionMessage &message,
	quint64 nGeneration)
{
	KSessionIncomingDispatchResult dispatchResult;
	if (message.type == CommandResultSessionMessageType)
	{
		handleCommandResult(message, nGeneration);
		dispatchResult.handlerResult = KProtocolHandlerResult::success();
		return dispatchResult;
	}

	if (KSessionMessageCodec::isCommand(message.type))
	{
		const auto cached = m_recentCommandResults.constFind(message.strRequestId);
		if (cached != m_recentCommandResults.constEnd())
		{
			transmit(cached.value());
			dispatchResult.handlerResult = KProtocolHandlerResult::success();
			return dispatchResult;
		}
	}

	dispatchResult.handlerResult = KProtocolHandlerResult::failure(
		ProtocolHandlerExecutionFailed, QStringLiteral("No session message handler"));
	const auto iterator = m_handlers.constFind(static_cast<int>(message.type));
	if (iterator != m_handlers.constEnd())
		dispatchResult.handlerResult = iterator.value()(message);
	if (!KSessionMessageCodec::isCommand(message.type))
		return dispatchResult;

	const KSessionMessage result = createCommandResult(message.strRequestId,
		dispatchResult.handlerResult);
	rememberCommandResult(result);
	transmit(result);
	dispatchResult.bEndSessionAccepted = message.type == EndSessionMessageType
		&& dispatchResult.handlerResult.status == ProtocolHandlerSucceeded;
	return dispatchResult;
}

void KSessionCommandDispatcher::failAll(const QString &strErrorCode)
{
	const QList<KPendingCommand> pendingCommands = m_pendingCommands.values();
	clear();
	for (const KPendingCommand &pending : pendingCommands)
	{
		emit commandCompleted(pending.message.type,
			pending.message.strRequestId,
			false,
			strErrorCode,
			pending.nGeneration);
	}
}

void KSessionCommandDispatcher::clear()
{
	m_pTimer->stop();
	m_pendingCommands.clear();
	m_recentCommandResults.clear();
	m_recentCommandResultIds.clear();
}

KSessionCommandTransmitResult KSessionCommandDispatcher::transmit(
	const KSessionMessage &message) const
{
	if (!m_transmitFunction)
		return { false, QStringLiteral("send_failed") };
	return m_transmitFunction(message);
}

void KSessionCommandDispatcher::handleTimer()
{
	const qint64 nNowMs = m_clock.elapsed();
	const QStringList requestIds = m_pendingCommands.keys();
	for (const QString &strRequestId : requestIds)
	{
		auto iterator = m_pendingCommands.find(strRequestId);
		if (iterator == m_pendingCommands.end())
			continue;
		KPendingCommand &pending = iterator.value();
		if (nNowMs - pending.nSentMs < kCommandTimeoutMs)
			continue;
		if (pending.nAttempts < kMaximumCommandAttempts)
		{
			const KSessionCommandTransmitResult result = transmit(pending.message);
			if (result.bAccepted)
			{
				++pending.nAttempts;
				pending.nSentMs = nNowMs;
				continue;
			}
			const KSessionMessage message = pending.message;
			const quint64 nGeneration = pending.nGeneration;
			m_pendingCommands.erase(iterator);
			emit commandCompleted(message.type,
				strRequestId,
				false,
				result.strErrorCode.isEmpty()
					? QStringLiteral("send_failed") : result.strErrorCode,
				nGeneration);
			continue;
		}

		const KSessionMessage message = pending.message;
		const quint64 nGeneration = pending.nGeneration;
		m_pendingCommands.erase(iterator);
		emit commandTimedOut(message.type, strRequestId, nGeneration);
	}
	if (m_pendingCommands.isEmpty())
		m_pTimer->stop();
}

void KSessionCommandDispatcher::handleCommandResult(const KSessionMessage &message,
	quint64 nGeneration)
{
	auto iterator = m_pendingCommands.find(message.strRequestId);
	if (iterator == m_pendingCommands.end()
		|| iterator->nGeneration != nGeneration)
	{
		return;
	}
	const KPendingCommand pending = iterator.value();
	m_pendingCommands.erase(iterator);
	if (m_pendingCommands.isEmpty())
		m_pTimer->stop();
	emit commandCompleted(pending.message.type,
		message.strRequestId,
		message.bSuccess,
		message.strErrorCode,
		pending.nGeneration);
}

KSessionMessage KSessionCommandDispatcher::createCommandResult(
	const QString &strRequestId,
	const KProtocolHandlerResult &handlerResult) const
{
	KSessionMessage result;
	result.type = CommandResultSessionMessageType;
	result.strRequestId = strRequestId;
	result.bSuccess = handlerResult.status == ProtocolHandlerSucceeded;
	if (result.bSuccess)
		return result;
	if (handlerResult.status == ProtocolHandlerInvalidState)
		result.strErrorCode = QStringLiteral("invalid_state");
	else if (handlerResult.status == ProtocolHandlerPermissionDenied)
		result.strErrorCode = QStringLiteral("permission_denied");
	else
		result.strErrorCode = QStringLiteral("execution_failed");
	return result;
}

void KSessionCommandDispatcher::rememberCommandResult(const KSessionMessage &message)
{
	if (m_recentCommandResults.contains(message.strRequestId))
		return;
	m_recentCommandResults.insert(message.strRequestId, message);
	m_recentCommandResultIds.enqueue(message.strRequestId);
	while (m_recentCommandResultIds.size() > kMaximumRecentCommandResults)
		m_recentCommandResults.remove(m_recentCommandResultIds.dequeue());
}
