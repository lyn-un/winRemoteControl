#ifndef _WINREMOTECONTROL_CURSORSHAPEMESSAGE_H_
#define _WINREMOTECONTROL_CURSORSHAPEMESSAGE_H_

#include "core/media/cursorshape.h"

#include <QtCore/QString>
#include <QtCore/QVector>

struct KProtocolEnvelope;

struct KCursorShapeMessage
{
	static constexpr int kMaximumChunkBytes = 8 * 1024;
	KCursorShape shape;
	int nOffset = 0;
	int nTotalBytes = 0;

	bool isValid() const;
};

Q_DECLARE_METATYPE(KCursorShapeMessage)

class KCursorShapeMessageCodec
{
public:
	static QVector<KCursorShapeMessage> split(const KCursorShape &shape);
	static QString encode(const KCursorShapeMessage &message);
	static bool decode(const QString &strMessage, KCursorShapeMessage *pMessage, QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KCursorShapeMessage *pMessage, QString *pErrorMessage);
	static QString typeName();
};

// Thread-confined, one assembly per session. Reset on every generation change.
class KCursorShapeAssembler
{
public:
	enum Result
	{
		IncompleteResult,
		CompleteResult,
		IgnoredResult,
		RejectedResult
	};
	static constexpr qint64 kTimeoutMs = 5000;

	Result accept(const KCursorShapeMessage &message, qint64 nNowMs,
		KCursorShape *pShape, QString *pErrorMessage);
	void expire(qint64 nNowMs);
	void reset();
	void discardPending();
	int pendingBytes() const;

private:
	void clearPending();
	KCursorShape m_pending;
	quint64 m_nHighestSequence = 0;
	qint64 m_nStartedMs = 0;
	int m_nTotalBytes = 0;
	bool m_bActive = false;
};

#endif // _WINREMOTECONTROL_CURSORSHAPEMESSAGE_H_
