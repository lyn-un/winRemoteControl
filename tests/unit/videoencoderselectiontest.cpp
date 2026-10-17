#include "codec/videoencoderselection.h"

#include <iostream>

int main()
{
	bool bPassed = true;
	const auto check = [&](bool bCondition, const char *pMessage)
	{
		if (!bCondition)
			std::cerr << pMessage << '\n';
		bPassed &= bCondition;
	};
	const KVideoAdapter intel{0, 0x8086, 1, 11, QStringLiteral("Intel"), false};
	const KVideoAdapter amd{1, 0x1002, 2, 22, QStringLiteral("AMD"), false};
	const KVideoAdapter nvidia{0, 0x10de, 3, 33, QStringLiteral("NVIDIA"), false};
	KVideoAdapter software = intel;
	software.bSoftware = true;
	auto candidates = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {amd, intel});
	check(candidates.size() == 3 && candidates[0].strEncoder == QStringLiteral("h264_qsv")
		&& candidates[0].adapter.nLuid == 11 && candidates[1].strEncoder == QStringLiteral("h264_amf")
		&& candidates[2].strEncoder == QStringLiteral("libx264"), "dual GPU capture order");
	KVideoAdapter secondIntel = intel;
	secondIntel.nIndex = 2;
	secondIntel.nLuid = 44;
	auto sameVendor = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {secondIntel, intel});
	check(sameVendor.size() == 3 && sameVendor[1].adapter.nLuid == 44, "same-vendor identity lost");
	const auto nvidiaCandidates = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {amd, nvidia});
	check(nvidiaCandidates.size() == 3 && nvidiaCandidates[0].strEncoder == QStringLiteral("h264_nvenc")
		&& nvidiaCandidates[0].adapter.nLuid == 33, "NVIDIA adapter order/identity");
	check(BuildVideoEncoderCandidates(NvencVideoEncoderPreference, {amd, intel}).empty(), "NVENC picked another vendor");
	check(BuildVideoEncoderCandidates(QuickSyncVideoEncoderPreference, {nvidia}).empty(), "QSV picked NVIDIA");
	check(BuildVideoEncoderCandidates(AmfVideoEncoderPreference, {nvidia}).empty(), "AMF picked NVIDIA");
	check(BuildVideoEncoderCandidates(NvencVideoEncoderPreference, {amd, nvidia}).front().strEncoder
		== QStringLiteral("h264_nvenc"), "manual NVIDIA selection");
	KVideoAdapter secondNvidia = nvidia;
	secondNvidia.nIndex = 3;
	secondNvidia.nLuid = 55;
	const auto nvidiaOnly = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {nvidia});
	check(nvidiaOnly.size() == 2 && nvidiaOnly.back().strEncoder == QStringLiteral("libx264"),
		"NVIDIA software fallback missing");
	const auto twoNvidia = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {secondNvidia, nvidia});
	check(twoNvidia.size() == 3 && twoNvidia[0].adapter.nLuid == 33 && twoNvidia[1].adapter.nLuid == 55,
		"multiple NVIDIA identity/order lost");
	KVideoAdapter unknown = nvidia;
	unknown.nVendorId = 0xffff;
	check(BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {unknown}).size() == 1,
		"unknown vendor selected as hardware");
	for (const auto &adapters : {std::vector<KVideoAdapter>{},
		std::vector<KVideoAdapter>{software}})
	{
		const auto softwareOnly = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, adapters);
		check(softwareOnly.size() == 1 && softwareOnly[0].strEncoder == QStringLiteral("libx264"),
			"unsupported adapter selected as hardware");
	}
	check(BuildVideoEncoderCandidates(QuickSyncVideoEncoderPreference, {amd}).empty(), "QSV picked AMD");
	check(BuildVideoEncoderCandidates(AmfVideoEncoderPreference, {intel}).empty(), "AMF picked Intel");
	check(BuildVideoEncoderCandidates(MediaFoundationVideoEncoderPreference, {}).front().strEncoder
		== QStringLiteral("h264_mf"), "manual MF removed");
	check(BuildVideoEncoderCandidates(LibX264VideoEncoderPreference, {intel}).size() == 1, "manual software changed");
	for (int nSuccessIndex = 0; nSuccessIndex <= 3; ++nSuccessIndex)
	{
		int nAttempts = 0;
		KVideoEncoderCandidate selected;
		QStringList failures;
		const bool bOpened = TryVideoEncoderCandidates(candidates,
			[&](const KVideoEncoderCandidate &, QString *pError)
			{
				*pError = QStringLiteral("mock initialization failure");
				return nAttempts++ == nSuccessIndex;
			}, &selected, &failures);
		check(bOpened == (nSuccessIndex < 3) && nAttempts == qMin(nSuccessIndex + 1, 3)
			&& failures.size() == qMin(nSuccessIndex, 3), "fallback execution order/error preservation");
		if (bOpened)
			check(selected.strEncoder == candidates[nSuccessIndex].strEncoder, "selected candidate mismatch");
	}
	int nManualAttempts = 0;
	QStringList failures;
	TryVideoEncoderCandidates(BuildVideoEncoderCandidates(QuickSyncVideoEncoderPreference, {intel, secondIntel}),
		[&](const KVideoEncoderCandidate &, QString *) { ++nManualAttempts; return false; }, nullptr, &failures);
	check(nManualAttempts == 1, "manual selection silently fell back");
	nManualAttempts = 0;
	TryVideoEncoderCandidates(BuildVideoEncoderCandidates(NvencVideoEncoderPreference, {secondNvidia, nvidia, amd}),
		[&](const KVideoEncoderCandidate &candidate, QString *pError)
		{
			++nManualAttempts;
			check(candidate.adapter.nLuid == nvidia.nLuid, "manual NVENC did not choose first NVIDIA");
			*pError = QStringLiteral("NVENC initialization failed");
			return false;
		}, nullptr, &failures);
	check(nManualAttempts == 1 && failures.size() == 1
		&& failures.front().contains(QStringLiteral("NVENC initialization failed")),
		"manual NVENC silently fell back or lost its error");
	KVideoAdapter thirdIntel = intel;
	thirdIntel.nIndex = 2;
	const auto mixedVendors = BuildVideoEncoderCandidates(AutoVideoEncoderPreference, {thirdIntel, amd, nvidia});
	check(mixedVendors.size() == 4 && mixedVendors[0].strEncoder == QStringLiteral("h264_nvenc")
		&& mixedVendors[1].strEncoder == QStringLiteral("h264_amf")
		&& mixedVendors[2].strEncoder == QStringLiteral("h264_qsv")
		&& mixedVendors[3].strEncoder == QStringLiteral("libx264"), "three-vendor fallback order");
	return bPassed ? 0 : 1;
}
