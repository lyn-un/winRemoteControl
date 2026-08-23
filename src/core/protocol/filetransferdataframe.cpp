#include "core/protocol/filetransferdataframe.h"

#include <QtCore/QUuid>

#include <limits>

namespace
{
	constexpr quint32 kMagic = 0x57524644U; // "WRFD" encoded big-endian.
	constexpr quint16 kVersion = 1;

	void AppendUint16(QByteArray *pData, quint16 nValue)
	{
		pData->append(static_cast<char>((nValue >> 8) & 0xff));
		pData->append(static_cast<char>(nValue & 0xff));
	}

	void AppendUint32(QByteArray *pData, quint32 nValue)
	{
		for (int nShift = 24; nShift >= 0; nShift -= 8)
			pData->append(static_cast<char>((nValue >> nShift) & 0xff));
	}

	void AppendUint64(QByteArray *pData, quint64 nValue)
	{
		for (int nShift = 56; nShift >= 0; nShift -= 8)
			pData->append(static_cast<char>((nValue >> nShift) & 0xff));
	}

	quint16 ReadUint16(const char *pData)
	{
		return (static_cast<quint16>(static_cast<quint8>(pData[0])) << 8)
			| static_cast<quint16>(static_cast<quint8>(pData[1]));
	}

	quint32 ReadUint32(const char *pData)
	{
		quint32 nValue = 0;
		for (int nIndex = 0; nIndex < 4; ++nIndex)
		{
			nValue = (nValue << 8)
				| static_cast<quint32>(static_cast<quint8>(pData[nIndex]));
		}
		return nValue;
	}

	quint64 ReadUint64(const char *pData)
	{
		quint64 nValue = 0;
		for (int nIndex = 0; nIndex < 8; ++nIndex)
		{
			nValue = (nValue << 8)
				| static_cast<quint64>(static_cast<quint8>(pData[nIndex]));
		}
		return nValue;
	}

	bool Fail(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}
}

QByteArray KFileTransferDataFrameCodec::encode(const KFileTransferDataFrame &frame,
	QString *pErrorMessage)
{
	const QUuid taskId(frame.strTaskId);
	const QUuid fileId(frame.strFileId);
	if (taskId.isNull() || fileId.isNull())
	{
		Fail(QStringLiteral("File transfer data id is invalid"), pErrorMessage);
		return {};
	}
	if (frame.payload.isEmpty() || frame.payload.size() > kMaximumPayloadBytes)
	{
		Fail(QStringLiteral("File transfer data payload length is invalid"), pErrorMessage);
		return {};
	}
	if (frame.nOffset > std::numeric_limits<quint64>::max()
		- static_cast<quint64>(frame.payload.size()))
	{
		Fail(QStringLiteral("File transfer data range overflows"), pErrorMessage);
		return {};
	}

	QByteArray data;
	data.reserve(kHeaderBytes + frame.payload.size());
	AppendUint32(&data, kMagic);
	AppendUint16(&data, kVersion);
	AppendUint16(&data, frame.nFlags);
	data.append(taskId.toRfc4122());
	data.append(fileId.toRfc4122());
	AppendUint64(&data, frame.nOffset);
	AppendUint32(&data, static_cast<quint32>(frame.payload.size()));
	data.append(frame.payload);
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return data;
}

bool KFileTransferDataFrameCodec::decode(const QByteArray &data,
	KFileTransferDataFrame *pFrame,
	QString *pErrorMessage)
{
	if (pFrame == nullptr)
		return Fail(QStringLiteral("File transfer data output is null"), pErrorMessage);
	*pFrame = KFileTransferDataFrame();
	if (data.size() < kHeaderBytes)
		return Fail(QStringLiteral("File transfer data frame is truncated"), pErrorMessage);
	if (data.size() > kMaximumFrameBytes)
		return Fail(QStringLiteral("File transfer data frame is too large"), pErrorMessage);

	const char *pData = data.constData();
	if (ReadUint32(pData) != kMagic)
		return Fail(QStringLiteral("File transfer data magic is invalid"), pErrorMessage);
	if (ReadUint16(pData + 4) != kVersion)
		return Fail(QStringLiteral("File transfer data version is unsupported"), pErrorMessage);
	const QUuid taskId = QUuid::fromRfc4122(data.mid(8, 16));
	const QUuid fileId = QUuid::fromRfc4122(data.mid(24, 16));
	if (taskId.isNull() || fileId.isNull())
		return Fail(QStringLiteral("File transfer data id is invalid"), pErrorMessage);
	const quint64 nOffset = ReadUint64(pData + 40);
	const quint32 nPayloadBytes = ReadUint32(pData + 48);
	if (nPayloadBytes == 0 || nPayloadBytes > kMaximumPayloadBytes)
	{
		return Fail(QStringLiteral("File transfer data payload length is invalid"),
			pErrorMessage);
	}
	if (data.size() != kHeaderBytes + static_cast<qsizetype>(nPayloadBytes))
		return Fail(QStringLiteral("File transfer data frame length mismatch"), pErrorMessage);
	if (nOffset > std::numeric_limits<quint64>::max() - nPayloadBytes)
		return Fail(QStringLiteral("File transfer data range overflows"), pErrorMessage);

	pFrame->nFlags = ReadUint16(pData + 6);
	pFrame->strTaskId = taskId.toString(QUuid::WithoutBraces);
	pFrame->strFileId = fileId.toString(QUuid::WithoutBraces);
	pFrame->nOffset = nOffset;
	pFrame->payload = data.mid(kHeaderBytes, nPayloadBytes);
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}
