#include "codec/videoencoderselection.h"

#include <algorithm>

QString VideoEncoderPreferenceName(KVideoEncoderPreference preference)
{
	switch (preference)
	{
	case AutoVideoEncoderPreference: return QStringLiteral("auto");
	case MediaFoundationVideoEncoderPreference: return QStringLiteral("h264_mf");
	case LibX264VideoEncoderPreference: return QStringLiteral("libx264");
	case QuickSyncVideoEncoderPreference: return QStringLiteral("h264_qsv");
	case AmfVideoEncoderPreference: return QStringLiteral("h264_amf");
	case NvencVideoEncoderPreference: return QStringLiteral("h264_nvenc");
	}
	return QStringLiteral("unknown");
}

std::vector<KVideoEncoderCandidate> BuildVideoEncoderCandidates(
	KVideoEncoderPreference preference, const std::vector<KVideoAdapter> &adapters)
{
	if (preference == LibX264VideoEncoderPreference)
		return {{QStringLiteral("libx264"), {}}};
	if (preference == MediaFoundationVideoEncoderPreference)
		return {{QStringLiteral("h264_mf"), {}}};
	auto orderedAdapters = adapters;
	std::stable_sort(orderedAdapters.begin(), orderedAdapters.end(),
		[](const KVideoAdapter &left, const KVideoAdapter &right) { return left.nIndex < right.nIndex; });
	std::vector<KVideoEncoderCandidate> candidates;
	for (const auto &adapter : orderedAdapters)
	{
		if (adapter.bSoftware || adapter.nIndex < 0)
			continue;
		QString strEncoder;
		if (adapter.nVendorId == 0x8086 && (preference == AutoVideoEncoderPreference || preference == QuickSyncVideoEncoderPreference))
			strEncoder = QStringLiteral("h264_qsv");
		else if (adapter.nVendorId == 0x1002 && (preference == AutoVideoEncoderPreference || preference == AmfVideoEncoderPreference))
			strEncoder = QStringLiteral("h264_amf");
		else if (adapter.nVendorId == 0x10de && (preference == AutoVideoEncoderPreference || preference == NvencVideoEncoderPreference))
			strEncoder = QStringLiteral("h264_nvenc");
		if (strEncoder.isEmpty())
			continue;
		candidates.push_back({strEncoder, adapter});
		if (preference != AutoVideoEncoderPreference)
			return candidates;
	}
	if (preference == AutoVideoEncoderPreference)
		candidates.push_back({QStringLiteral("libx264"), {}});
	return candidates;
}

bool TryVideoEncoderCandidates(const std::vector<KVideoEncoderCandidate> &candidates,
	const std::function<bool(const KVideoEncoderCandidate &, QString *)> &openCandidate,
	KVideoEncoderCandidate *pSelected, QStringList *pFailures)
{
	if (pFailures)
		pFailures->clear();
	for (const auto &candidate : candidates)
	{
		QString strError;
		if (openCandidate(candidate, &strError))
		{
			if (pSelected)
				*pSelected = candidate;
			return true;
		}
		if (pFailures)
			pFailures->append(QStringLiteral("%1(adapter=%2): %3")
				.arg(candidate.strEncoder).arg(candidate.adapter.nIndex).arg(strError));
	}
	return false;
}
