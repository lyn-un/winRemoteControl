#include "core/terminal/terminalrelayframecodec.h"
#include "core/terminal/terminalrelayprotocol.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

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

	void TestFragmentedAndCoalescedFrames()
	{
		const QByteArray first = KTerminalRelayFrameCodec::encode(
			KTerminalRelayProtocol::HelloFrameType, QByteArray("token"));
		const QByteArray second = KTerminalRelayFrameCodec::encode(
			KTerminalRelayProtocol::InputFrameType, QByteArray("dir\r"));
		KTerminalRelayFrameCodec codec;
		QVector<KTerminalRelayFrame> frames;
		Check(codec.append(first.left(5), &frames),
			QStringLiteral("fragment prefix is accepted"));
		Check(frames.isEmpty(), QStringLiteral("incomplete frame is retained"));
		Check(codec.append(first.mid(5) + second, &frames),
			QStringLiteral("fragment tail and coalesced frame are accepted"));
		Check(frames.size() == 2, QStringLiteral("two complete frames are decoded"));
		Check(frames.at(0).payload == QByteArray("token"),
			QStringLiteral("hello payload survives framing"));
		Check(frames.at(1).payload == QByteArray("dir\r"),
			QStringLiteral("input payload survives framing"));
	}

	void TestInvalidAndOversizedHeaders()
	{
		auto header = KTerminalRelayProtocol::EncodeFrameHeader(
			KTerminalRelayProtocol::InputFrameType, 0);
		header[4] = static_cast<std::uint8_t>(KTerminalRelayProtocol::kVersion + 1);
		QByteArray invalid(reinterpret_cast<const char *>(header.data()), header.size());
		KTerminalRelayFrameCodec codec;
		QVector<KTerminalRelayFrame> frames;
		QString strError;
		Check(!codec.append(invalid, &frames, &strError),
			QStringLiteral("unknown relay protocol version is rejected"));

		header = KTerminalRelayProtocol::EncodeFrameHeader(
			KTerminalRelayProtocol::InputFrameType,
			KTerminalRelayProtocol::kMaximumPayloadBytes + 1);
		QByteArray oversized(reinterpret_cast<const char *>(header.data()), header.size());
		Check(!codec.append(oversized, &frames, &strError),
			QStringLiteral("oversized relay frame is rejected"));

		header = KTerminalRelayProtocol::EncodeFrameHeader(99, 0);
		QByteArray unknown(reinterpret_cast<const char *>(header.data()), header.size());
		Check(!codec.append(unknown, &frames, &strError),
			QStringLiteral("unknown relay frame type is rejected"));
		Check(!codec.append(QByteArray(1024 * 1024 + 1, 'x'), &frames, &strError),
			QStringLiteral("relay receive buffer is bounded"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestFragmentedAndCoalescedFrames();
	TestInvalidAndOversizedHeaders();
	return g_nFailureCount == 0 ? 0 : 1;
}
