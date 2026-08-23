#include "core/protocol/filetransferdataframe.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

#include <limits>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	KFileTransferDataFrame ValidFrame()
	{
		KFileTransferDataFrame frame;
		frame.nFlags = 0x0102;
		frame.strTaskId = QStringLiteral("6ba7b810-9dad-41d1-80b4-00c04fd430c8");
		frame.strFileId = QStringLiteral("7ca76045-2ded-4f65-9912-16ff5ee3d0cc");
		frame.nOffset = 0x0102030405060708ULL;
		frame.payload = QByteArray("abc");
		return frame;
	}

	void TestRoundTripAndWireOrder()
	{
		const KFileTransferDataFrame source = ValidFrame();
		const QByteArray encoded = KFileTransferDataFrameCodec::encode(source);
		KFileTransferDataFrame decoded;
		Check(KFileTransferDataFrameCodec::decode(encoded, &decoded)
			&& decoded.nFlags == source.nFlags
			&& decoded.strTaskId == source.strTaskId
			&& decoded.strFileId == source.strFileId
			&& decoded.nOffset == source.nOffset
			&& decoded.payload == source.payload,
			QStringLiteral("file transfer data frame round-trips"));
		Check(encoded.left(4) == QByteArray("WRFD")
			&& static_cast<quint8>(encoded.at(4)) == 0
			&& static_cast<quint8>(encoded.at(5)) == 1
			&& static_cast<quint8>(encoded.at(6)) == 1
			&& static_cast<quint8>(encoded.at(7)) == 2
			&& static_cast<quint8>(encoded.at(40)) == 1
			&& static_cast<quint8>(encoded.at(47)) == 8
			&& static_cast<quint8>(encoded.at(51)) == 3,
			QStringLiteral("file transfer data header uses network byte order"));
	}

	void TestInvalidFrames()
	{
		const KFileTransferDataFrame source = ValidFrame();
		const QByteArray encoded = KFileTransferDataFrameCodec::encode(source);
		KFileTransferDataFrame decoded;

		QByteArray malformed = encoded;
		malformed[0] = 'X';
		Check(!KFileTransferDataFrameCodec::decode(malformed, &decoded),
			QStringLiteral("invalid data magic is rejected"));
		malformed = encoded;
		malformed[5] = 2;
		Check(!KFileTransferDataFrameCodec::decode(malformed, &decoded),
			QStringLiteral("unsupported data version is rejected"));
		Check(!KFileTransferDataFrameCodec::decode(encoded.left(encoded.size() - 1),
			&decoded), QStringLiteral("truncated data payload is rejected"));
		Check(!KFileTransferDataFrameCodec::decode(encoded + QByteArray("x"), &decoded),
			QStringLiteral("trailing data bytes are rejected"));

		malformed = encoded;
		for (int nIndex = 8; nIndex < 24; ++nIndex)
			malformed[nIndex] = 0;
		Check(!KFileTransferDataFrameCodec::decode(malformed, &decoded),
			QStringLiteral("null task UUID is rejected"));

		KFileTransferDataFrame invalid = source;
		invalid.payload.clear();
		Check(KFileTransferDataFrameCodec::encode(invalid).isEmpty(),
			QStringLiteral("empty data payload is rejected"));
		invalid = source;
		invalid.payload = QByteArray(KFileTransferDataFrameCodec::kMaximumPayloadBytes + 1,
			'x');
		Check(KFileTransferDataFrameCodec::encode(invalid).isEmpty(),
			QStringLiteral("oversized data payload is rejected"));
		invalid = source;
		invalid.nOffset = std::numeric_limits<quint64>::max();
		Check(KFileTransferDataFrameCodec::encode(invalid).isEmpty(),
			QStringLiteral("overflowing data range is rejected"));
	}

	void TestMaximumFrame()
	{
		KFileTransferDataFrame source = ValidFrame();
		source.nOffset = 0;
		source.payload = QByteArray(KFileTransferDataFrameCodec::kMaximumPayloadBytes, 'x');
		const QByteArray encoded = KFileTransferDataFrameCodec::encode(source);
		KFileTransferDataFrame decoded;
		Check(encoded.size() == KFileTransferDataFrameCodec::kMaximumFrameBytes
			&& KFileTransferDataFrameCodec::decode(encoded, &decoded)
			&& decoded.payload == source.payload,
			QStringLiteral("maximum-sized data frame round-trips"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestRoundTripAndWireOrder();
	TestInvalidFrames();
	TestMaximumFrame();
	return g_nFailureCount == 0 ? 0 : 1;
}
