#ifndef _WINREMOTECONTROL_CORE_TRANSPORT_OUTBOUNDMESSAGEQUEUE_H_
#define _WINREMOTECONTROL_CORE_TRANSPORT_OUTBOUNDMESSAGEQUEUE_H_

#include <QtCore/QList>
#include <QtCore/QString>

struct KOutboundMessage
{
	QString strPayload;
	QString strCoalescingKey;
	bool bReliable = true;
};

enum KOutboundEnqueueResult
{
	EnqueuedOutboundMessage,
	CoalescedOutboundMessage,
	DroppedOutboundMessage,
	OverflowOutboundMessage
};

class KOutboundMessageQueue
{
public:
	KOutboundMessageQueue(int nMaximumMessages, int nMaximumBytes);

	KOutboundEnqueueResult enqueue(const KOutboundMessage &message);
	bool takeFirst(KOutboundMessage *pMessage);
	void prepend(const KOutboundMessage &message);
	void clear();
	bool isEmpty() const;
	int messageCount() const;
	int byteCount() const;

private:
	bool makeRoomForReliable(int nRequiredBytes);
	static int messageBytes(const KOutboundMessage &message);

	QList<KOutboundMessage> m_messages;
	int m_nMaximumMessages = 0;
	int m_nMaximumBytes = 0;
	int m_nBytes = 0;
};

#endif // _WINREMOTECONTROL_CORE_TRANSPORT_OUTBOUNDMESSAGEQUEUE_H_
