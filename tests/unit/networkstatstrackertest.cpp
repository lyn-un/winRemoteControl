#include "core/transport/networkstatstracker.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

#include <cmath>

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void testStatsDeltasAndReset()
	{
		KNetworkStatsTracker tracker;
		KNetworkStatsSample first;
		first.bHasInboundVideo = true;
		first.nTimestampMs = 1000;
		first.nBytesReceived = 100000;
		first.nPacketsReceived = 100;
		first.nPacketsLost = 1;
		first.nRttMs = 15;
		first.nJitterMs = 4;
		first.nJitterBufferEmittedCount = 10;
		first.fJitterBufferDelaySeconds = 0.1;
		first.fJitterBufferTargetDelaySeconds = 0.2;
		first.nFramesDecoded = 10;
		first.fTotalDecodeTimeSeconds = 0.05;
		const KNetworkStats firstStats = tracker.update(first, 8);
		check(firstStats.nBitrateKbps == 0,
			QStringLiteral("first sample has no bitrate delta"));
		check(firstStats.strQuality == QStringLiteral("excellent"),
			QStringLiteral("low latency sample is excellent"));

		KNetworkStatsSample second = first;
		second.nTimestampMs = 2000;
		second.nBytesReceived += 125000;
		second.nPacketsReceived += 99;
		second.nPacketsLost += 1;
		second.nJitterBufferEmittedCount += 10;
		second.fJitterBufferDelaySeconds += 0.2;
		second.fJitterBufferTargetDelaySeconds += 0.3;
		second.nFramesDecoded += 10;
		second.fTotalDecodeTimeSeconds += 0.1;
		const KNetworkStats secondStats = tracker.update(second, 9);
		check(secondStats.nBitrateKbps == 1000,
			QStringLiteral("byte delta produces bitrate"));
		check(std::abs(secondStats.fPacketLossRate - 0.01) < 0.0001,
			QStringLiteral("packet deltas produce loss rate"));
		check(secondStats.nJitterBufferDelayMs == 20,
			QStringLiteral("jitter buffer average is calculated"));
		check(secondStats.nJitterBufferTargetDelayMs == 30,
			QStringLiteral("target jitter buffer average is calculated"));
		check(secondStats.nDecodeTimeMs == 10,
			QStringLiteral("per-frame decode time is calculated"));

		tracker.reset();
		const KNetworkStats resetStats = tracker.update(second, 9);
		check(resetStats.nBitrateKbps == 0,
			QStringLiteral("reset removes previous delta history"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	testStatsDeltasAndReset();
	return g_nFailureCount == 0 ? 0 : 1;
}
