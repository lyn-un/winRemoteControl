#include "codec/h264encoder.h"

#include <Windows.h>
#include <psapi.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <algorithm>
#include <iostream>
#include <set>
#include <vector>

namespace
{
struct KResources
{
	DWORD nHandles = 0;
	SIZE_T nPrivateBytes = 0;
	ULONGLONG nCpu100ns = 0;
};

KResources Snapshot()
{
	KResources result;
	PROCESS_MEMORY_COUNTERS_EX memory = {};
	memory.cb = sizeof(memory);
	GetProcessHandleCount(GetCurrentProcess(), &result.nHandles);
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)))
		result.nPrivateBytes = memory.PrivateUsage;
	FILETIME creation, exit, kernel, user;
	if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
		result.nCpu100ns = (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime
			+ (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
	return result;
}

bool RunCycle(KVideoEncoderPreference preference, int nWidth, int nHeight, int nFrames, bool bPaced)
{
	KH264Encoder encoder(preference);
	QString strError;
	std::set<std::int64_t> outputPts;
	bool bUnique = true;
	quint64 nBytes = 0;
	if (!encoder.openStream(nWidth, nHeight, 60, 6000, [](const QByteArray &) {}, &strError))
	{
		std::cerr << strError.toStdString() << '\n';
		return false;
	}
	encoder.setPacketCallback([&](const QByteArray &packet, std::int64_t nPts)
		{
			bUnique = outputPts.insert(nPts).second && bUnique;
			nBytes += packet.size();
		});
	std::vector<unsigned char> y(nWidth * nHeight), uv(nWidth * nHeight / 4, 128);
	std::vector<qint64> costs;
	const KResources start = Snapshot();
	QElapsedTimer timer;
	timer.start();
	for (int nFrame = 0; nFrame < nFrames; ++nFrame)
	{
		for (int nRow = 0; nRow < nHeight; ++nRow)
			for (int nCol = 0; nCol < nWidth; ++nCol)
				y[nRow * nWidth + nCol] = static_cast<unsigned char>(16 + ((nCol / 8 + nRow / 8 + nFrame) % 220));
		if (bPaced)
		{
			const qint64 nWaitUs = nFrame * 1000000LL / 60 - timer.nsecsElapsed() / 1000;
			if (nWaitUs > 0)
				QThread::usleep(static_cast<unsigned long>(nWaitUs));
		}
		const qint64 nStartNs = timer.nsecsElapsed();
		if (!encoder.encodeI420Frame(y.data(), nWidth, uv.data(), nWidth / 2, uv.data(), nWidth / 2,
			nWidth, nHeight, 1000 + nFrame * 1000LL / 60, nFrame == 0, &strError))
		{
			std::cerr << strError.toStdString() << '\n';
			return false;
		}
		costs.push_back(timer.nsecsElapsed() - nStartNs);
	}
	if (!encoder.close(&strError) || !bUnique || outputPts.size() != static_cast<size_t>(nFrames)
		|| *outputPts.begin() != 0 || *outputPts.rbegin() != nFrames - 1)
		return false;
	const KResources finish = Snapshot();
	std::sort(costs.begin(), costs.end());
	std::cout << "resourceCycle width=" << nWidth << " height=" << nHeight << " inputFps=60 paced=" << bPaced
		<< " inputFrames=" << nFrames << " outputFrames=" << outputPts.size()
		<< " encodeCallP50Ms=" << costs[costs.size() / 2] / 1e6
		<< " encodeCallP99Ms=" << costs[costs.size() * 99 / 100] / 1e6
		<< " cpuMs=" << (finish.nCpu100ns - start.nCpu100ns) / 10000.0
		<< " wallMs=" << timer.elapsed() << " bytes=" << nBytes
		<< " handlesAfterClose=" << finish.nHandles << " privateBytesAfterClose=" << finish.nPrivateBytes << std::endl;
	return true;
}
}

bool TestHardwareResources(KVideoEncoderPreference preference)
{
	// Warm up process-wide driver caches before observing repeated lifecycle costs.
	if (!RunCycle(preference, 1280, 720, 60, false) || !RunCycle(preference, 1920, 1080, 60, false))
		return false;
	const KResources baseline = Snapshot();
	if (!RunCycle(preference, 1920, 1080, 600, true))
		return false;
	for (int nCycle = 0; nCycle < 20; ++nCycle)
		if (!RunCycle(preference, nCycle % 2 ? 1280 : 1920, nCycle % 2 ? 720 : 1080, 60, false))
			return false;
	const KResources finish = Snapshot();
	std::cout << "resourceSummary cycles=20 baselineHandles=" << baseline.nHandles
		<< " finalHandles=" << finish.nHandles << " baselinePrivateBytes=" << baseline.nPrivateBytes
		<< " finalPrivateBytes=" << finish.nPrivateBytes << " observationOnly=1" << std::endl;
	return true;
}
