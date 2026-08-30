#ifndef _WINREMOTECONTROL_TOOLS_DIAGNOSTICS_RESOURCEPROBECOMMON_H_
#define _WINREMOTECONTROL_TOOLS_DIAGNOSTICS_RESOURCEPROBECOMMON_H_

#include "common/resourcetracelogger.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <iostream>

#include <Windows.h>
#include <winternl.h>

#include <map>
#include <algorithm>
#include <vector>

namespace ResourceProbeDetail
{
	constexpr SYSTEM_INFORMATION_CLASS kSystemExtendedHandleInformation =
		static_cast<SYSTEM_INFORMATION_CLASS>(64);
	constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xc0000004L);

	struct KSystemHandleEntry
	{
		void *pObject = nullptr;
		ULONG_PTR nProcessId = 0;
		ULONG_PTR nHandle = 0;
		ULONG nGrantedAccess = 0;
		USHORT nCreatorBackTraceIndex = 0;
		USHORT nObjectTypeIndex = 0;
		ULONG nHandleAttributes = 0;
		ULONG nReserved = 0;
	};

	struct KSystemHandleInformation
	{
		ULONG_PTR nHandleCount = 0;
		ULONG_PTR nReserved = 0;
		KSystemHandleEntry entries[1];
	};
}

inline QJsonObject CurrentProcessHandleTypes()
{
	using NtQuerySystemInformationFunction = NTSTATUS(NTAPI *)(
		SYSTEM_INFORMATION_CLASS, void *, ULONG, ULONG *);
	using NtQueryObjectFunction = NTSTATUS(NTAPI *)(
		HANDLE, OBJECT_INFORMATION_CLASS, void *, ULONG, ULONG *);
	const auto pQuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFunction>(
		::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
	const auto pQueryObject = reinterpret_cast<NtQueryObjectFunction>(
		::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
	if (pQuerySystemInformation == nullptr || pQueryObject == nullptr)
		return {};

	std::vector<unsigned char> vecBuffer(256 * 1024);
	ULONG nRequiredLength = 0;
	NTSTATUS status = 0;
	do
	{
		status = pQuerySystemInformation(
			ResourceProbeDetail::kSystemExtendedHandleInformation,
			vecBuffer.data(), static_cast<ULONG>(vecBuffer.size()), &nRequiredLength);
		if (status == ResourceProbeDetail::kStatusInfoLengthMismatch)
			vecBuffer.resize(std::max<std::size_t>(vecBuffer.size() * 2, nRequiredLength));
	}
	while (status == ResourceProbeDetail::kStatusInfoLengthMismatch);
	if (status < 0)
		return {};

	const auto *pInformation = reinterpret_cast<
		const ResourceProbeDetail::KSystemHandleInformation *>(vecBuffer.data());
	std::map<USHORT, int> counts;
	std::map<USHORT, HANDLE> representativeHandles;
	const ULONG_PTR nCurrentProcessId = ::GetCurrentProcessId();
	for (ULONG_PTR nIndex = 0; nIndex < pInformation->nHandleCount; ++nIndex)
	{
		const auto &entry = pInformation->entries[nIndex];
		if (entry.nProcessId == nCurrentProcessId)
		{
			++counts[entry.nObjectTypeIndex];
			representativeHandles.try_emplace(entry.nObjectTypeIndex,
				reinterpret_cast<HANDLE>(entry.nHandle));
		}
	}

	QJsonObject result;
	for (const auto &[nTypeIndex, nCount] : counts)
	{
		std::vector<unsigned char> vecTypeBuffer(4096);
		ULONG nTypeLength = 0;
		QString strTypeName;
		const NTSTATUS typeStatus = pQueryObject(representativeHandles[nTypeIndex],
			static_cast<OBJECT_INFORMATION_CLASS>(2), vecTypeBuffer.data(),
			static_cast<ULONG>(vecTypeBuffer.size()), &nTypeLength);
		if (typeStatus >= 0)
		{
			const auto *pTypeName = reinterpret_cast<const UNICODE_STRING *>(vecTypeBuffer.data());
			if (pTypeName->Buffer != nullptr && pTypeName->Length > 0)
				strTypeName = QString::fromWCharArray(
					pTypeName->Buffer, pTypeName->Length / sizeof(wchar_t));
		}
		const QString strKey = strTypeName.isEmpty()
			? QString::number(nTypeIndex)
			: QStringLiteral("%1:%2").arg(nTypeIndex).arg(strTypeName);
		result.insert(strKey, nCount);
	}
	return result;
}

inline void PrintResourceProbeSnapshot(const QString &strProbe,
	int nCycle,
	const QString &strStage)
{
	const KProcessResourceSnapshot snapshot = KResourceTraceLogger::snapshot();
	QJsonObject result;
	result.insert(QStringLiteral("probe"), strProbe);
	result.insert(QStringLiteral("cycle"), nCycle);
	result.insert(QStringLiteral("stage"), strStage);
	result.insert(QStringLiteral("privateBytes"), QString::number(snapshot.nPrivateBytes));
	result.insert(QStringLiteral("workingSetBytes"), QString::number(snapshot.nWorkingSetBytes));
	result.insert(QStringLiteral("handles"), static_cast<int>(snapshot.nHandleCount));
	result.insert(QStringLiteral("threads"), static_cast<int>(snapshot.nThreadCount));
	result.insert(QStringLiteral("gpuDedicatedBytes"),
		QString::number(snapshot.nGpuDedicatedBytes));
	result.insert(QStringLiteral("gpuSharedBytes"),
		QString::number(snapshot.nGpuSharedBytes));
	result.insert(QStringLiteral("handleTypes"), CurrentProcessHandleTypes());
	std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
}

#endif // _WINREMOTECONTROL_TOOLS_DIAGNOSTICS_RESOURCEPROBECOMMON_H_
