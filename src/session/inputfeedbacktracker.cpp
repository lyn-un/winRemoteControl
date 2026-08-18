#include "session/inputfeedbacktracker.h"

#include "common/latencytracelogger.h"

#include <algorithm>

namespace
{
	constexpr qint64 kInputMoveTraceIntervalMs = 500;
	constexpr qsizetype kMaxPendingInputTraceCount = 2048;
	constexpr qsizetype kInputRoundTripWindowSize = 120;
	constexpr quint64 kInputRoundTripStatsInterval = 30;
}

bool KInputFeedbackTracker::shouldTraceMouseMove()
{
	if (!KLatencyTraceLogger::isEnabled())
		return false;

	if (!m_moveTraceTimer.isValid())
	{
		m_moveTraceTimer.start();
		return true;
	}

	if (m_moveTraceTimer.elapsed() < kInputMoveTraceIntervalMs)
		return false;

	m_moveTraceTimer.restart();
	return true;
}

void KInputFeedbackTracker::recordInputSent(const KInputMessage &message)
{
	if (!KLatencyTraceLogger::isEnabled())
		return;

	if (!m_roundTripTimer.isValid())
		m_roundTripTimer.start();
	while (m_sentTraces.size() >= kMaxPendingInputTraceCount)
		m_sentTraces.erase(m_sentTraces.begin());

	KPendingInputTrace trace;
	trace.nSentMs = m_roundTripTimer.elapsed();
	trace.strType = KInputMessageCodec::typeName(message.type);
	trace.bKeyPressed = message.bPressed;
	m_sentTraces.insert(message.nSequence, trace);
}

void KInputFeedbackTracker::handleRendered(quint64 nSeq)
{
	if (!KLatencyTraceLogger::isEnabled() || !m_roundTripTimer.isValid() || nSeq == 0)
		return;

	const auto sentTraceIt = m_sentTraces.constFind(nSeq);
	const bool bFoundSentTrace = sentTraceIt != m_sentTraces.constEnd();
	const KPendingInputTrace sentTrace =
		bFoundSentTrace ? sentTraceIt.value() : KPendingInputTrace();
	for (auto iterator = m_sentTraces.begin(); iterator != m_sentTraces.end();)
	{
		const bool bSameSequenceSpace = iterator.key() % 2 == nSeq % 2;
		if (bSameSequenceSpace && iterator.key() <= nSeq)
			iterator = m_sentTraces.erase(iterator);
		else
			++iterator;
	}

	if (!bFoundSentTrace)
		return;

	const qint64 nRoundTripMs = m_roundTripTimer.elapsed() - sentTrace.nSentMs;
	if (nRoundTripMs < 0)
		return;

	const bool bKeyInput = sentTrace.strType == KInputMessageCodec::typeName(KeyInputMessageType);
	const bool bIncludeInStats = !bKeyInput || sentTrace.bKeyPressed;
	const QString strPressed = bKeyInput
		? QStringLiteral(" pressed=%1").arg(sentTrace.bKeyPressed ? 1 : 0)
		: QString();
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		QStringLiteral("input_roundtrip"),
		QStringLiteral("seq=%1 type=%2%3 roundTripMs=%4 includedInStats=%5")
			.arg(nSeq)
			.arg(sentTrace.strType)
			.arg(strPressed)
			.arg(nRoundTripMs)
			.arg(bIncludeInStats ? 1 : 0));

	if (!bIncludeInStats)
		return;
	if (m_roundTripSamples.size() >= kInputRoundTripWindowSize)
		m_roundTripSamples.remove(0);
	m_roundTripSamples.append(nRoundTripMs);
	++m_nRoundTripSampleCount;
	if (m_nRoundTripSampleCount % kInputRoundTripStatsInterval == 0)
		logRoundTripStats(QStringLiteral("input_roundtrip_stats"));
}

void KInputFeedbackTracker::reset()
{
	if (KLatencyTraceLogger::isEnabled() && !m_roundTripSamples.isEmpty())
		logRoundTripStats(QStringLiteral("input_roundtrip_summary"));
	m_sentTraces.clear();
	m_roundTripSamples.clear();
	m_nRoundTripSampleCount = 0;
	m_roundTripTimer.invalidate();
}

void KInputFeedbackTracker::logRoundTripStats(const QString &strEventName)
{
	if (m_roundTripSamples.isEmpty())
		return;

	QVector<qint64> sortedSamples = m_roundTripSamples;
	std::sort(sortedSamples.begin(), sortedSamples.end());
	qint64 nTotalMs = 0;
	for (const qint64 nSampleMs : sortedSamples)
		nTotalMs += nSampleMs;

	const qsizetype nP50Index = (sortedSamples.size() - 1) * 50 / 100;
	const qsizetype nP95Index = (sortedSamples.size() * 95 + 99) / 100 - 1;
	const qint64 nAverageMs = (nTotalMs + sortedSamples.size() / 2) / sortedSamples.size();
	KLatencyTraceLogger::write(QStringLiteral("controller"),
		strEventName,
		QStringLiteral("scope=exclude_key_release samples=%1 totalSamples=%2 avgMs=%3 p50Ms=%4 p95Ms=%5 maxMs=%6")
			.arg(sortedSamples.size())
			.arg(m_nRoundTripSampleCount)
			.arg(nAverageMs)
			.arg(sortedSamples.at(nP50Index))
			.arg(sortedSamples.at(nP95Index))
			.arg(sortedSamples.constLast()));
}
