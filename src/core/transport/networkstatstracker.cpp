#include "core/transport/networkstatstracker.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr int kExcellentRttMs = 20;
	constexpr int kGoodRttMs = 60;
	constexpr int kExcellentJitterMs = 10;
	constexpr int kGoodJitterMs = 30;
	constexpr double kExcellentPacketLossRate = 0.01;
	constexpr double kGoodPacketLossRate = 0.03;
	constexpr int kBitsPerByte = 8;
	constexpr int kMsPerSecond = 1000;
	constexpr int kBitsPerKilobit = 1000;
	constexpr double kSecondsToMilliseconds = 1000.0;

	int secondsToMs(double fSeconds)
	{
		return static_cast<int>(std::round(fSeconds * kSecondsToMilliseconds));
	}

	int clampFrameCount(qint64 nFrameCount)
	{
		return static_cast<int>(std::min<qint64>(
			nFrameCount,
			std::numeric_limits<int>::max()));
	}
}

KNetworkStats KNetworkStatsTracker::update(const KNetworkStatsSample &sample,
	int nDataChannelRttMs)
{
	KNetworkStats stats;
	stats.nRttMs = sample.nRttMs;
	stats.nJitterMs = qMax(0, sample.nJitterMs);
	stats.nFps = sample.nFps;
	stats.nDataChannelRttMs = nDataChannelRttMs;
	stats.nFramesDecoded = clampFrameCount(sample.nFramesDecoded);
	stats.nKeyFramesDecoded = clampFrameCount(sample.nKeyFramesDecoded);
	stats.nFramesDropped = clampFrameCount(sample.nFramesDropped);

	if (sample.bHasInboundVideo
		&& m_bHasPreviousSample
		&& sample.nTimestampMs > m_previousSample.nTimestampMs)
	{
		const qint64 nElapsedMs = sample.nTimestampMs - m_previousSample.nTimestampMs;
		if (sample.nBytesReceived >= m_previousSample.nBytesReceived)
		{
			const quint64 nBytesDelta =
				sample.nBytesReceived - m_previousSample.nBytesReceived;
			stats.nBitrateKbps = static_cast<int>(
				(nBytesDelta * kBitsPerByte * kMsPerSecond)
				/ (static_cast<quint64>(nElapsedMs) * kBitsPerKilobit));
		}

		const qint64 nReceivedDelta =
			sample.nPacketsReceived - m_previousSample.nPacketsReceived;
		const qint64 nLostDelta = sample.nPacketsLost - m_previousSample.nPacketsLost;
		const qint64 nTotalPacketsDelta = std::max<qint64>(0, nReceivedDelta)
			+ std::max<qint64>(0, nLostDelta);
		if (nTotalPacketsDelta > 0)
		{
			stats.fPacketLossRate = static_cast<double>(std::max<qint64>(0, nLostDelta))
				/ static_cast<double>(nTotalPacketsDelta);
		}

		const quint64 nJitterEmittedDelta = sample.nJitterBufferEmittedCount
			>= m_previousSample.nJitterBufferEmittedCount
			? sample.nJitterBufferEmittedCount - m_previousSample.nJitterBufferEmittedCount
			: 0;
		if (nJitterEmittedDelta > 0)
		{
			stats.nJitterBufferDelayMs = secondsToMs(
				std::max(0.0,
					sample.fJitterBufferDelaySeconds
					- m_previousSample.fJitterBufferDelaySeconds)
				/ static_cast<double>(nJitterEmittedDelta));
			stats.nJitterBufferTargetDelayMs = secondsToMs(
				std::max(0.0,
					sample.fJitterBufferTargetDelaySeconds
					- m_previousSample.fJitterBufferTargetDelaySeconds)
				/ static_cast<double>(nJitterEmittedDelta));
		}

		const qint64 nFramesDecodedDelta =
			sample.nFramesDecoded - m_previousSample.nFramesDecoded;
		if (nFramesDecodedDelta > 0)
		{
			stats.nDecodeTimeMs = secondsToMs(
				std::max(0.0,
					sample.fTotalDecodeTimeSeconds
					- m_previousSample.fTotalDecodeTimeSeconds)
				/ static_cast<double>(nFramesDecodedDelta));
		}
	}

	if (sample.bHasInboundVideo)
	{
		m_bHasPreviousSample = true;
		m_previousSample = sample;
	}
	stats.strQuality = evaluateQuality(stats);
	return stats;
}

void KNetworkStatsTracker::reset()
{
	m_bHasPreviousSample = false;
	m_previousSample = KNetworkStatsSample();
}

QString KNetworkStatsTracker::evaluateQuality(const KNetworkStats &stats)
{
	if (stats.nRttMs < 0)
		return QStringLiteral("unknown");
	if (stats.nRttMs < kExcellentRttMs
		&& stats.fPacketLossRate < kExcellentPacketLossRate
		&& stats.nJitterMs < kExcellentJitterMs)
	{
		return QStringLiteral("excellent");
	}
	if (stats.nRttMs < kGoodRttMs
		&& stats.fPacketLossRate < kGoodPacketLossRate
		&& stats.nJitterMs < kGoodJitterMs)
	{
		return QStringLiteral("good");
	}
	return QStringLiteral("poor");
}
