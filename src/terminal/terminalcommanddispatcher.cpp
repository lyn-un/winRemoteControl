#include "terminal/terminalcommanddispatcher.h"

#include <QtCore/QTimer>
#include <QtCore/QUuid>

namespace
{
	constexpr int kCommandTimeoutMs = 1000;
	constexpr int kMaximumAttempts = 2;
	constexpr int kMaximumPendingCommands = 128;
	constexpr int kMaximumRecentResults = 128;
	constexpr qsizetype kMaximumPendingBytes = 256 * 1024;
	constexpr qsizetype kMaximumRecentResultBytes = 256 * 1024;
}

KTerminalCommandDispatcher::KTerminalCommandDispatcher(QObject *pParent)
	: QObject(pParent)
	, m_pTimer(new QTimer(this))
{
	m_pTimer->setInterval(100);
	m_clock.start();
	connect(m_pTimer, &QTimer::timeout, this,
		&KTerminalCommandDispatcher::handleTimer);
}

void KTerminalCommandDispatcher::setTransmitFunction(TransmitFunction transmitFunction)
{
	m_transmitFunction = std::move(transmitFunction);
}

void KTerminalCommandDispatcher::setHandler(Handler handler)
{
	m_handler = std::move(handler);
}

bool KTerminalCommandDispatcher::send(KTerminalMessage message, quint64 nGeneration)
{
	if (!m_transmitFunction)
		return false;
	if (!KTerminalMessageCodec::isReliableCommand(message.type))
		return m_transmitFunction(message);
	if (message.type == ResizeTerminalMessageType && hasPendingResize())
	{
		message.strCommandId.clear();
		m_deferredResize = message;
		m_nDeferredResizeGeneration = nGeneration;
		return true;
	}
	if (message.strCommandId.isEmpty())
		message.strCommandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const qsizetype nEncodedBytes = KTerminalMessageCodec::encode(message).toUtf8().size();
	if (nEncodedBytes <= 0 || m_pending.size() >= kMaximumPendingCommands
		|| m_nPendingBytes + nEncodedBytes > kMaximumPendingBytes)
	{
		return false;
	}
	KPendingCommand pending;
	pending.message = message;
	pending.nSentMs = m_clock.elapsed();
	pending.nAttempts = 1;
	pending.nGeneration = nGeneration;
	pending.nEncodedBytes = nEncodedBytes;
	m_pending.insert(message.strCommandId, pending);
	m_nPendingBytes += nEncodedBytes;
	if (!m_transmitFunction(message))
	{
		auto iterator = m_pending.find(message.strCommandId);
		if (iterator != m_pending.end())
		{
			m_nPendingBytes -= iterator->nEncodedBytes;
			m_pending.erase(iterator);
		}
		return false;
	}
	if (!m_pending.contains(message.strCommandId))
		return true;
	if (!m_pTimer->isActive())
		m_pTimer->start();
	return true;
}

void KTerminalCommandDispatcher::handleIncoming(
	const KTerminalMessage &message, quint64 nGeneration)
{
	if (message.type == CommandResultTerminalMessageType)
	{
		auto iterator = m_pending.find(message.strCommandId);
		if (iterator == m_pending.end() || iterator->nGeneration != nGeneration
			|| iterator->message.strRequestId != message.strRequestId)
		{
			return;
		}
		const KPendingCommand pending = iterator.value();
		m_nPendingBytes -= pending.nEncodedBytes;
		m_pending.erase(iterator);
		emit commandCompleted(pending.message.type, message.strRequestId,
			message.strCommandId, message.bSuccess, message.strErrorCode, nGeneration);
		if (pending.message.type == ResizeTerminalMessageType)
			sendDeferredResize();
		return;
	}
	if (!KTerminalMessageCodec::isReliableCommand(message.type))
	{
		if (m_handler)
			m_handler(message, nullptr);
		return;
	}
	const auto cached = m_recentResults.constFind(message.strCommandId);
	if (cached != m_recentResults.constEnd())
	{
		m_transmitFunction(cached.value());
		return;
	}
	QString strErrorCode;
	const bool bSuccess = m_handler && m_handler(message, &strErrorCode);
	KTerminalMessage result;
	result.type = CommandResultTerminalMessageType;
	result.strRequestId = message.strRequestId;
	result.strCommandId = message.strCommandId;
	result.bSuccess = bSuccess;
	result.strErrorCode = bSuccess ? QString() : (strErrorCode.isEmpty()
		? QStringLiteral("invalid_state") : strErrorCode);
	rememberResult(result);
	m_transmitFunction(result);
}

void KTerminalCommandDispatcher::clear()
{
	m_pTimer->stop();
	m_pending.clear();
	m_nPendingBytes = 0;
	m_recentResults.clear();
	m_recentResultIds.clear();
	m_nRecentResultBytes = 0;
	m_deferredResize = KTerminalMessage();
	m_nDeferredResizeGeneration = 0;
}

void KTerminalCommandDispatcher::handleTimer()
{
	const qint64 nNowMs = m_clock.elapsed();
	for (const QString &strCommandId : m_pending.keys())
	{
		auto iterator = m_pending.find(strCommandId);
		if (iterator == m_pending.end()
			|| nNowMs - iterator->nSentMs < kCommandTimeoutMs)
		{
			continue;
		}
		if (iterator->nAttempts < kMaximumAttempts)
		{
			const KTerminalMessage retryMessage = iterator->message;
			const bool bSent = m_transmitFunction(retryMessage);
			iterator = m_pending.find(strCommandId);
			if (iterator == m_pending.end())
				continue;
			if (bSent)
			{
				++iterator->nAttempts;
				iterator->nSentMs = nNowMs;
				continue;
			}
		}
		const KPendingCommand pending = iterator.value();
		m_nPendingBytes -= pending.nEncodedBytes;
		m_pending.erase(iterator);
		emit commandTimedOut(pending.message.type, pending.message.strRequestId,
			pending.message.strCommandId, pending.nGeneration);
		if (pending.message.type == ResizeTerminalMessageType)
			sendDeferredResize();
	}
	if (m_pending.isEmpty())
		m_pTimer->stop();
}

bool KTerminalCommandDispatcher::hasPendingResize() const
{
	for (auto iterator = m_pending.cbegin(); iterator != m_pending.cend(); ++iterator)
	{
		if (iterator->message.type == ResizeTerminalMessageType)
			return true;
	}
	return false;
}

void KTerminalCommandDispatcher::sendDeferredResize()
{
	if (m_deferredResize.type != ResizeTerminalMessageType)
		return;
	const KTerminalMessage message = m_deferredResize;
	const quint64 nGeneration = m_nDeferredResizeGeneration;
	m_deferredResize = KTerminalMessage();
	m_nDeferredResizeGeneration = 0;
	QTimer::singleShot(0, this, [this, message, nGeneration]()
		{ send(message, nGeneration); });
}

void KTerminalCommandDispatcher::rememberResult(const KTerminalMessage &result)
{
	const qsizetype nEncodedBytes = KTerminalMessageCodec::encode(result).toUtf8().size();
	if (nEncodedBytes <= 0 || nEncodedBytes > kMaximumRecentResultBytes)
		return;
	m_recentResults.insert(result.strCommandId, result);
	m_recentResultIds.enqueue(result.strCommandId);
	m_nRecentResultBytes += nEncodedBytes;
	while (m_recentResultIds.size() > kMaximumRecentResults
		|| m_nRecentResultBytes > kMaximumRecentResultBytes)
	{
		const QString strOldestId = m_recentResultIds.dequeue();
		const auto iterator = m_recentResults.find(strOldestId);
		if (iterator == m_recentResults.end())
			continue;
		m_nRecentResultBytes -= KTerminalMessageCodec::encode(
			iterator.value()).toUtf8().size();
		m_recentResults.erase(iterator);
	}
}
