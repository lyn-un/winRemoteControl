#include "core/protocol/cursorshapemessage.h"

#include <QtCore/QJsonObject>

#include <algorithm>

#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

namespace
{
bool Fail(const QString &strError, QString *pErrorMessage)
{
	if (pErrorMessage != nullptr)
		*pErrorMessage = strError;
	return false;
}

bool ReadInteger(const QJsonObject &object, const char *pName, int *pValue)
{
	const QJsonValue value = object.value(QString::fromLatin1(pName));
	const int nValue = value.toInt(-1);
	if (!value.isDouble() || value.toDouble() != static_cast<double>(nValue) || nValue < 0)
		return false;
	*pValue = nValue;
	return true;
}
}

bool KCursorShapeMessage::isValid() const
{
	if (shape.nSequence == 0 || nTotalBytes < 0 || shape.expectedByteCount() != nTotalBytes
		|| nOffset < 0 || nOffset > nTotalBytes || nOffset % kMaximumChunkBytes != 0)
	{
		return false;
	}
	if (nTotalBytes != 0 && nOffset == nTotalBytes)
		return false;
	return shape.data.size() == std::min(kMaximumChunkBytes, nTotalBytes - nOffset);
}

QVector<KCursorShapeMessage> KCursorShapeMessageCodec::split(const KCursorShape &shape)
{
	QVector<KCursorShapeMessage> messages;
	if (!shape.isValid())
		return messages;
	int nOffset = 0;
	do
	{
		KCursorShapeMessage message;
		message.shape = shape;
		message.shape.nGeneration = 0;
		message.shape.data = shape.data.mid(nOffset, KCursorShapeMessage::kMaximumChunkBytes);
		message.nOffset = nOffset;
		message.nTotalBytes = static_cast<int>(shape.data.size());
		messages.push_back(message);
		nOffset += KCursorShapeMessage::kMaximumChunkBytes;
	}
	while (nOffset < shape.data.size());
	return messages;
}

QString KCursorShapeMessageCodec::encode(const KCursorShapeMessage &message)
{
	if (!message.isValid())
		return QString();
	const KCursorShape &shape = message.shape;
	QJsonObject payload;
	payload.insert(QStringLiteral("kind"), static_cast<int>(shape.kind));
	payload.insert(QStringLiteral("width"), shape.nWidth);
	payload.insert(QStringLiteral("height"), shape.nHeight);
	payload.insert(QStringLiteral("stride"), shape.nStride);
	payload.insert(QStringLiteral("hotspotX"), shape.nHotspotX);
	payload.insert(QStringLiteral("hotspotY"), shape.nHotspotY);
	payload.insert(QStringLiteral("visible"), shape.bVisible);
	payload.insert(QStringLiteral("offset"), message.nOffset);
	payload.insert(QStringLiteral("totalBytes"), message.nTotalBytes);
	payload.insert(QStringLiteral("data"), QString::fromLatin1(shape.data.toBase64()));
	return KProtocolEnvelopeCodec::encode(SessionProtocolChannel, typeName(), QString(), shape.nSequence, payload);
}

bool KCursorShapeMessageCodec::decode(const QString &strMessage,
	KCursorShapeMessage *pMessage, QString *pErrorMessage)
{
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SessionProtocolChannel, strMessage, &envelope, pErrorMessage))
		return false;
	return decode(envelope, pMessage, pErrorMessage);
}

bool KCursorShapeMessageCodec::decode(const KProtocolEnvelope &envelope,
	KCursorShapeMessage *pMessage, QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("Cursor shape output is null"), pErrorMessage);
	*pMessage = {};
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	if (envelope.channel != SessionProtocolChannel || envelope.nVersion != KProtocolConstraints::kEnvelopeSchemaVersion
		|| envelope.strType != typeName() || !envelope.strRequestId.isEmpty() || envelope.nSequence == 0)
	{
		return Fail(QStringLiteral("Cursor shape envelope is invalid"), pErrorMessage);
	}
	KCursorShapeMessage message;
	KCursorShape &shape = message.shape;
	shape.nSequence = envelope.nSequence;
	int nKind = 0;
	const QJsonObject &payload = envelope.payload;
	if (!ReadInteger(payload, "kind", &nKind) || nKind > MaskedColorCursorShape
		|| !ReadInteger(payload, "width", &shape.nWidth) || !ReadInteger(payload, "height", &shape.nHeight)
		|| !ReadInteger(payload, "stride", &shape.nStride) || !ReadInteger(payload, "hotspotX", &shape.nHotspotX)
		|| !ReadInteger(payload, "hotspotY", &shape.nHotspotY) || !ReadInteger(payload, "offset", &message.nOffset)
		|| !ReadInteger(payload, "totalBytes", &message.nTotalBytes) || !payload.value(QStringLiteral("visible")).isBool())
	{
		return Fail(QStringLiteral("Cursor shape metadata is invalid"), pErrorMessage);
	}
	shape.kind = static_cast<KCursorShapeKind>(nKind);
	shape.bVisible = payload.value(QStringLiteral("visible")).toBool();
	const QJsonValue dataValue = payload.value(QStringLiteral("data"));
	const int nMaximumBase64 = ((KCursorShapeMessage::kMaximumChunkBytes + 2) / 3) * 4;
	if (!dataValue.isString() || dataValue.toString().size() > nMaximumBase64)
		return Fail(QStringLiteral("Cursor shape chunk exceeds limit"), pErrorMessage);
	const QByteArray encoded = dataValue.toString().toLatin1();
	const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
	// Canonical Base64 rejects whitespace, non-ASCII substitutions and incomplete padding.
	if (!decoded || decoded.decoded.toBase64() != encoded)
		return Fail(QStringLiteral("Cursor shape Base64 is invalid"), pErrorMessage);
	shape.data = decoded.decoded;
	if (!message.isValid())
		return Fail(QStringLiteral("Cursor shape dimensions or chunk range is invalid"), pErrorMessage);
	*pMessage = message;
	return true;
}

QString KCursorShapeMessageCodec::typeName()
{
	return QStringLiteral("cursorShape");
}

KCursorShapeAssembler::Result KCursorShapeAssembler::accept(const KCursorShapeMessage &message,
	qint64 nNowMs, KCursorShape *pShape, QString *pErrorMessage)
{
	if (pShape != nullptr)
		*pShape = {};
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	if (pShape == nullptr || nNowMs < 0 || !message.isValid())
	{
		Fail(QStringLiteral("Invalid cursor shape fragment"), pErrorMessage);
		return RejectedResult;
	}
	expire(nNowMs);
	if (message.shape.nSequence < m_nHighestSequence
		|| (message.shape.nSequence == m_nHighestSequence && !m_bActive))
	{
		return IgnoredResult;
	}
	if (message.shape.nSequence > m_nHighestSequence)
	{
		if (message.nOffset != 0)
		{
			Fail(QStringLiteral("Cursor shape starts with a missing fragment"), pErrorMessage);
			return RejectedResult;
		}
		clearPending();
		m_pending = message.shape;
		m_pending.nGeneration = 0;
		m_pending.data.clear();
		m_nHighestSequence = message.shape.nSequence;
		m_nStartedMs = nNowMs;
		m_nTotalBytes = message.nTotalBytes;
		m_bActive = true;
	}
	if (!m_pending.hasSameMetadata(message.shape) || m_nTotalBytes != message.nTotalBytes
		|| message.nOffset != m_pending.data.size() || nNowMs < m_nStartedMs)
	{
		clearPending();
		Fail(QStringLiteral("Cursor shape fragments are inconsistent or out of order"), pErrorMessage);
		return RejectedResult;
	}
	m_pending.data.append(message.shape.data);
	if (m_pending.data.size() != m_nTotalBytes)
		return IncompleteResult;
	*pShape = m_pending;
	clearPending();
	return CompleteResult;
}

void KCursorShapeAssembler::expire(qint64 nNowMs)
{
	if (m_bActive && nNowMs >= m_nStartedMs && nNowMs - m_nStartedMs >= kTimeoutMs)
		clearPending();
}

void KCursorShapeAssembler::reset()
{
	clearPending();
	m_nHighestSequence = 0;
}

void KCursorShapeAssembler::discardPending()
{
	clearPending();
}

int KCursorShapeAssembler::pendingBytes() const
{
	return static_cast<int>(m_pending.data.size());
}

void KCursorShapeAssembler::clearPending()
{
	m_pending = {};
	m_nTotalBytes = 0;
	m_nStartedMs = 0;
	m_bActive = false;
}
