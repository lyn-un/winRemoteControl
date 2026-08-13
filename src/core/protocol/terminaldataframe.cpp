#include "core/protocol/terminaldataframe.h"

#include <QtCore/QUuid>

namespace
{
	constexpr quint32 kMagic = 0x54445257U; // "WRDT" encoded little-endian.
	constexpr quint16 kVersion = 1;

	void AppendUint16(QByteArray *pData, quint16 nValue)
	{
		pData->append(static_cast<char>(nValue & 0xff));
		pData->append(static_cast<char>((nValue >> 8) & 0xff));
	}

	void AppendUint32(QByteArray *pData, quint32 nValue)
	{
		for (int nShift = 0; nShift < 32; nShift += 8)
			pData->append(static_cast<char>((nValue >> nShift) & 0xff));
	}

	void AppendUint64(QByteArray *pData, quint64 nValue)
	{
		for (int nShift = 0; nShift < 64; nShift += 8)
			pData->append(static_cast<char>((nValue >> nShift) & 0xff));
	}

	quint16 ReadUint16(const char *pData)
	{
		return static_cast<quint16>(static_cast<quint8>(pData[0]))
			| (static_cast<quint16>(static_cast<quint8>(pData[1])) << 8);
	}

	quint32 ReadUint32(const char *pData)
	{
		quint32 nValue = 0;
		for (int nIndex = 0; nIndex < 4; ++nIndex)
			nValue |= static_cast<quint32>(static_cast<quint8>(pData[nIndex]))
				<< (nIndex * 8);
		return nValue;
	}

	quint64 ReadUint64(const char *pData)
	{
		quint64 nValue = 0;
		for (int nIndex = 0; nIndex < 8; ++nIndex)
			nValue |= static_cast<quint64>(static_cast<quint8>(pData[nIndex]))
				<< (nIndex * 8);
		return nValue;
	}

	bool Fail(const QString &strMessage, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strMessage;
		return false;
	}
}

QByteArray KTerminalDataFrameCodec::encode(
	const KTerminalDataFrame &frame,
	QString *pErrorMessage)
{
	const QUuid requestId(frame.strRequestId);
	if (requestId.isNull())
	{
		Fail(QStringLiteral("invalid terminal requestId"), pErrorMessage);
		return {};
	}
	if (frame.direction != InputTerminalDataDirection
		&& frame.direction != OutputTerminalDataDirection)
	{
		Fail(QStringLiteral("invalid terminal data direction"), pErrorMessage);
		return {};
	}
	if (frame.nSequence == 0)
	{
		Fail(QStringLiteral("invalid terminal data sequence"), pErrorMessage);
		return {};
	}
	if (frame.payload.isEmpty() || frame.payload.size() > kMaximumPayloadBytes)
	{
		Fail(QStringLiteral("invalid terminal data payload length"), pErrorMessage);
		return {};
	}

	QByteArray data;
	data.reserve(kHeaderBytes + frame.payload.size());
	AppendUint32(&data, kMagic);
	AppendUint16(&data, kVersion);
	AppendUint16(&data, static_cast<quint16>(frame.direction));
	data.append(requestId.toRfc4122());
	AppendUint64(&data, frame.nSequence);
	AppendUint32(&data, static_cast<quint32>(frame.payload.size()));
	data.append(frame.payload);
	return data;
}

bool KTerminalDataFrameCodec::decode(
	const QByteArray &data,
	KTerminalDataFrame *pFrame,
	QString *pErrorMessage)
{
	if (pFrame == nullptr)
		return Fail(QStringLiteral("terminal data output is null"), pErrorMessage);
	if (data.size() < kHeaderBytes)
		return Fail(QStringLiteral("terminal data frame is truncated"), pErrorMessage);
	const char *pData = data.constData();
	if (ReadUint32(pData) != kMagic)
		return Fail(QStringLiteral("invalid terminal data magic"), pErrorMessage);
	if (ReadUint16(pData + 4) != kVersion)
		return Fail(QStringLiteral("unsupported terminal data version"), pErrorMessage);
	const quint16 nDirection = ReadUint16(pData + 6);
	if (nDirection != InputTerminalDataDirection
		&& nDirection != OutputTerminalDataDirection)
	{
		return Fail(QStringLiteral("invalid terminal data direction"), pErrorMessage);
	}
	const QUuid requestId = QUuid::fromRfc4122(data.mid(8, 16));
	if (requestId.isNull())
		return Fail(QStringLiteral("invalid terminal requestId"), pErrorMessage);
	const quint64 nSequence = ReadUint64(pData + 24);
	if (nSequence == 0)
		return Fail(QStringLiteral("invalid terminal data sequence"), pErrorMessage);
	const quint32 nPayloadBytes = ReadUint32(pData + 32);
	if (nPayloadBytes == 0 || nPayloadBytes > kMaximumPayloadBytes)
		return Fail(QStringLiteral("invalid terminal data payload length"), pErrorMessage);
	if (data.size() != kHeaderBytes + static_cast<qsizetype>(nPayloadBytes))
		return Fail(QStringLiteral("terminal data frame length mismatch"), pErrorMessage);

	pFrame->direction = static_cast<KTerminalDataDirection>(nDirection);
	pFrame->strRequestId = requestId.toString(QUuid::WithoutBraces);
	pFrame->nSequence = nSequence;
	pFrame->payload = data.mid(kHeaderBytes, nPayloadBytes);
	return true;
}
