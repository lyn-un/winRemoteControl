#include "core/transport/outboundmessagequeue.h"

KOutboundMessageQueue::KOutboundMessageQueue(int nMaximumMessages, int nMaximumBytes)
	: m_nMaximumMessages(nMaximumMessages)
	, m_nMaximumBytes(nMaximumBytes)
{
}

KOutboundEnqueueResult KOutboundMessageQueue::enqueue(const KOutboundMessage &message)
{
	const int nBytes = messageBytes(message);
	if (!message.strCoalescingKey.isEmpty())
	{
		for (int i = m_messages.size() - 1; i >= 0; --i)
		{
			if (m_messages.at(i).strCoalescingKey != message.strCoalescingKey)
				continue;
			const int nNewTotal = m_nBytes - messageBytes(m_messages.at(i)) + nBytes;
			if (nNewTotal > m_nMaximumBytes)
				return DroppedOutboundMessage;
			m_nBytes -= messageBytes(m_messages.at(i));
			m_messages.removeAt(i);
			m_messages.append(message);
			m_nBytes += nBytes;
			return CoalescedOutboundMessage;
		}
	}

	if ((m_messages.size() >= m_nMaximumMessages || m_nBytes + nBytes > m_nMaximumBytes)
		&& (!message.bReliable || !makeRoomForReliable(nBytes)))
	{
		return message.bReliable ? OverflowOutboundMessage : DroppedOutboundMessage;
	}

	m_messages.append(message);
	m_nBytes += nBytes;
	return EnqueuedOutboundMessage;
}

bool KOutboundMessageQueue::takeFirst(KOutboundMessage *pMessage)
{
	if (pMessage == nullptr || m_messages.isEmpty())
		return false;
	*pMessage = m_messages.takeFirst();
	m_nBytes -= messageBytes(*pMessage);
	return true;
}

void KOutboundMessageQueue::prepend(const KOutboundMessage &message)
{
	m_messages.prepend(message);
	m_nBytes += messageBytes(message);
}

void KOutboundMessageQueue::clear()
{
	m_messages.clear();
	m_nBytes = 0;
}

bool KOutboundMessageQueue::isEmpty() const
{
	return m_messages.isEmpty();
}

int KOutboundMessageQueue::messageCount() const
{
	return m_messages.size();
}

int KOutboundMessageQueue::byteCount() const
{
	return m_nBytes;
}

bool KOutboundMessageQueue::makeRoomForReliable(int nRequiredBytes)
{
	for (int i = 0; i < m_messages.size()
		&& (m_messages.size() >= m_nMaximumMessages
			|| m_nBytes + nRequiredBytes > m_nMaximumBytes);)
	{
		if (m_messages.at(i).strCoalescingKey.isEmpty())
		{
			++i;
			continue;
		}
		m_nBytes -= messageBytes(m_messages.at(i));
		m_messages.removeAt(i);
	}
	return m_messages.size() < m_nMaximumMessages
		&& m_nBytes + nRequiredBytes <= m_nMaximumBytes;
}

int KOutboundMessageQueue::messageBytes(const KOutboundMessage &message)
{
	return message.strPayload.toUtf8().size();
}
