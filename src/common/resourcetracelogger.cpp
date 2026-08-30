#include "common/resourcetracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QTextStream>

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

namespace
{
	constexpr qint64 kResourceTraceMaxFileSize = 5 * 1024 * 1024;

	QMutex g_resourceTraceMutex;
	bool g_bResourceTraceEnabled = false;
	QString g_strResourceTraceLogDirectory;
	std::once_flag g_gpuAdaptersInitializationFlag;
	std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter3>> g_vecGpuAdapters;

	QString LogFilePath(const QString &strRole)
	{
		return QDir(g_strResourceTraceLogDirectory).absoluteFilePath(
			QStringLiteral("resource_trace_%1.log").arg(strRole));
	}

	void RotateIfNeeded(const QString &strFilePath)
	{
		QFile file(strFilePath);
		if (!file.exists() || file.size() < kResourceTraceMaxFileSize)
			return;

		const QString strOldFilePath = strFilePath + QStringLiteral(".old");
		QFile::remove(strOldFilePath);
		file.rename(strOldFilePath);
	}

	quint32 CurrentProcessThreadCount()
	{
		const DWORD dwProcessId = ::GetCurrentProcessId();
		const HANDLE hSnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (hSnapshot == INVALID_HANDLE_VALUE)
			return 0;

		quint32 nCount = 0;
		THREADENTRY32 entry = {};
		entry.dwSize = sizeof(entry);
		if (::Thread32First(hSnapshot, &entry))
		{
			do
			{
				if (entry.th32OwnerProcessID == dwProcessId)
					++nCount;
			}
			while (::Thread32Next(hSnapshot, &entry));
		}
		::CloseHandle(hSnapshot);
		return nCount;
	}

	void InitializeGpuAdapters()
	{
		Microsoft::WRL::ComPtr<IDXGIFactory1> spFactory;
		if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&spFactory))))
			return;

		for (UINT nIndex = 0; ; ++nIndex)
		{
			Microsoft::WRL::ComPtr<IDXGIAdapter1> spAdapter;
			if (spFactory->EnumAdapters1(nIndex, &spAdapter) == DXGI_ERROR_NOT_FOUND)
				break;

			Microsoft::WRL::ComPtr<IDXGIAdapter3> spAdapter3;
			if (FAILED(spAdapter.As(&spAdapter3)))
				continue;
			g_vecGpuAdapters.push_back(std::move(spAdapter3));
		}
	}

	void QueryGpuUsage(KProcessResourceSnapshot *pSnapshot)
	{
		std::call_once(g_gpuAdaptersInitializationFlag, InitializeGpuAdapters);

		quint64 nDedicatedBytes = 0;
		quint64 nSharedBytes = 0;
		bool bFoundAdapter = false;
		for (const auto &spAdapter3 : g_vecGpuAdapters)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
			DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo = {};
			if (SUCCEEDED(spAdapter3->QueryVideoMemoryInfo(
				0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo)))
			{
				nDedicatedBytes += localInfo.CurrentUsage;
				bFoundAdapter = true;
			}
			if (SUCCEEDED(spAdapter3->QueryVideoMemoryInfo(
				0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo)))
			{
				nSharedBytes += nonLocalInfo.CurrentUsage;
				bFoundAdapter = true;
			}
		}

		pSnapshot->nGpuDedicatedBytes = nDedicatedBytes;
		pSnapshot->nGpuSharedBytes = nSharedBytes;
		pSnapshot->bGpuAvailable = bFoundAdapter;
	}
}

void KResourceTraceLogger::configure(bool bEnabled, const QString &strLogDirectory)
{
	QMutexLocker locker(&g_resourceTraceMutex);
	g_bResourceTraceEnabled = bEnabled;
	g_strResourceTraceLogDirectory = QDir::cleanPath(strLogDirectory);
	if (g_bResourceTraceEnabled && !QDir().mkpath(g_strResourceTraceLogDirectory))
	{
		qWarning().noquote() << QStringLiteral("Unable to create resource trace directory: %1")
			.arg(g_strResourceTraceLogDirectory);
		g_bResourceTraceEnabled = false;
	}
}

bool KResourceTraceLogger::isEnabled()
{
	QMutexLocker locker(&g_resourceTraceMutex);
	return g_bResourceTraceEnabled;
}

KProcessResourceSnapshot KResourceTraceLogger::snapshot()
{
	KProcessResourceSnapshot result;
	PROCESS_MEMORY_COUNTERS_EX counters = {};
	counters.cb = sizeof(counters);
	if (::GetProcessMemoryInfo(::GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
	{
		result.nPrivateBytes = counters.PrivateUsage;
		result.nWorkingSetBytes = counters.WorkingSetSize;
		result.bMemoryAvailable = true;
	}

	DWORD dwHandleCount = 0;
	if (::GetProcessHandleCount(::GetCurrentProcess(), &dwHandleCount))
	{
		result.nHandleCount = dwHandleCount;
		result.bHandleCountAvailable = true;
	}

	result.nThreadCount = CurrentProcessThreadCount();
	result.bThreadCountAvailable = result.nThreadCount > 0;
	QueryGpuUsage(&result);
	return result;
}

void KResourceTraceLogger::write(const QString &strRole,
	const QString &strStage,
	quint64 nGeneration,
	bool bStale)
{
	if (!isEnabled())
		return;

	const KProcessResourceSnapshot resources = snapshot();
	QMutexLocker locker(&g_resourceTraceMutex);
	if (!g_bResourceTraceEnabled)
		return;

	const QString strFilePath = LogFilePath(strRole);
	RotateIfNeeded(strFilePath);
	QFile file(strFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
	{
		qWarning().noquote() << QStringLiteral("Unable to open resource trace log: %1")
			.arg(strFilePath);
		return;
	}

	QTextStream stream(&file);
	stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
		<< QStringLiteral(" [RESOURCE_TRACE]")
		<< QStringLiteral(" pid=") << ::GetCurrentProcessId()
		<< QStringLiteral(" role=") << strRole
		<< QStringLiteral(" generation=") << nGeneration
		<< QStringLiteral(" stage=") << strStage
		<< QStringLiteral(" stale=") << (bStale ? 1 : 0)
		<< QStringLiteral(" privateBytes=") << resources.nPrivateBytes
		<< QStringLiteral(" workingSetBytes=") << resources.nWorkingSetBytes
		<< QStringLiteral(" handles=") << resources.nHandleCount
		<< QStringLiteral(" threads=") << resources.nThreadCount
		<< QStringLiteral(" gpuDedicatedBytes=") << resources.nGpuDedicatedBytes
		<< QStringLiteral(" gpuSharedBytes=") << resources.nGpuSharedBytes
		<< QStringLiteral(" memoryAvailable=") << (resources.bMemoryAvailable ? 1 : 0)
		<< QStringLiteral(" gpuAvailable=") << (resources.bGpuAvailable ? 1 : 0)
		<< Qt::endl;
}
