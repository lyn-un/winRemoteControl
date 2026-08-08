#include "capture/captureframesink.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cstring>
#include <iostream>

extern "C"
{
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace
{
	struct KProcessSample
	{
		quint64 nCpu100Ns = 0;
		quint64 nPeakWorkingSetBytes = 0;
	};

	quint64 FileTimeValue(const FILETIME &fileTime)
	{
		ULARGE_INTEGER value;
		value.LowPart = fileTime.dwLowDateTime;
		value.HighPart = fileTime.dwHighDateTime;
		return value.QuadPart;
	}

	KProcessSample ProcessSample()
	{
		KProcessSample sample;
		// Force a scheduling boundary so GetProcessTimes includes the work from
		// the just-completed time slice instead of returning a stale quantum.
		Sleep(1);
		FILETIME creationTime;
		FILETIME exitTime;
		FILETIME kernelTime;
		FILETIME userTime;
		if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime,
			&kernelTime, &userTime))
		{
			sample.nCpu100Ns = FileTimeValue(kernelTime) + FileTimeValue(userTime);
		}

		PROCESS_MEMORY_COUNTERS counters = {};
		counters.cb = sizeof(counters);
		if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
			sample.nPeakWorkingSetBytes = counters.PeakWorkingSetSize;
		return sample;
	}

	bool ReadPositiveArgument(const QStringList &arguments,
		const QString &strName,
		int nDefaultValue,
		int *pValue)
	{
		if (pValue == nullptr)
			return false;
		const int nIndex = arguments.indexOf(strName);
		if (nIndex < 0)
		{
			*pValue = nDefaultValue;
			return true;
		}
		if (nIndex + 1 >= arguments.size())
			return false;
		bool bOk = false;
		const int nValue = arguments.at(nIndex + 1).toInt(&bOk);
		if (!bOk || nValue <= 0)
			return false;
		*pValue = nValue;
		return true;
	}

	QJsonObject CommonResult(const QString &strMode,
		int nWidth,
		int nHeight,
		int nFrames,
		qint64 nWallUs,
		quint64 nCpu100Ns,
		quint64 nPeakWorkingSetBytes)
	{
		QJsonObject result;
		result.insert(QStringLiteral("mode"), strMode);
		result.insert(QStringLiteral("width"), nWidth);
		result.insert(QStringLiteral("height"), nHeight);
		result.insert(QStringLiteral("frames"), nFrames);
		result.insert(QStringLiteral("wallUs"), static_cast<double>(nWallUs));
		const quint64 nCpuUs = nCpu100Ns / 10;
		result.insert(QStringLiteral("averageFrameUs"),
			static_cast<double>(nWallUs) / nFrames);
		result.insert(QStringLiteral("cpuUs"), static_cast<double>(nCpuUs));
		result.insert(QStringLiteral("singleCoreCpuPercent"), nWallUs > 0
			? static_cast<double>(nCpuUs) * 100.0 / nWallUs : 0.0);
		result.insert(QStringLiteral("peakWorkingSetBytes"),
			static_cast<double>(nPeakWorkingSetBytes));
		return result;
	}

	QJsonObject RunLegacy(KCaptureFrame *pFrame, int nFrames)
	{
		const int nWidth = pFrame->nWidth & ~1;
		const int nHeight = pFrame->nHeight & ~1;
		const int nYBytes = nWidth * nHeight;
		const int nChromaBytes = (nWidth / 2) * (nHeight / 2);
		const quint64 nCopiedBytesPerFrame = static_cast<quint64>(
			nYBytes + nChromaBytes * 2);
		quint64 nChecksum = 0;
		const KProcessSample before = ProcessSample();
		QElapsedTimer timer;
		timer.start();

		for (int nFrame = 0; nFrame < nFrames; ++nFrame)
		{
			QByteArray yPlane;
			QByteArray uPlane;
			QByteArray vPlane;
			yPlane.resize(nYBytes);
			uPlane.resize(nChromaBytes);
			vPlane.resize(nChromaBytes);

			SwsContext *pContext = sws_getContext(pFrame->nWidth,
				pFrame->nHeight,
				AV_PIX_FMT_BGRA,
				nWidth,
				nHeight,
				AV_PIX_FMT_YUV420P,
				SWS_FAST_BILINEAR,
				nullptr,
				nullptr,
				nullptr);
			if (pContext == nullptr)
				return {};
			const uint8_t *pSource[] = {
				pFrame->vecBgraBuffer.data(), nullptr, nullptr, nullptr
			};
			const int nSourceStride[] = { pFrame->nWidth * 4, 0, 0, 0 };
			uint8_t *pDestination[] = {
				reinterpret_cast<uint8_t *>(yPlane.data()),
				reinterpret_cast<uint8_t *>(uPlane.data()),
				reinterpret_cast<uint8_t *>(vPlane.data()),
				nullptr
			};
			const int nDestinationStride[] = { nWidth, nWidth / 2, nWidth / 2, 0 };
			const int nRows = sws_scale(pContext,
				pSource,
				nSourceStride,
				0,
				pFrame->nHeight,
				pDestination,
				nDestinationStride);
			sws_freeContext(pContext);
			if (nRows != nHeight)
				return {};

			QByteArray copiedI420;
			copiedI420.resize(nYBytes + nChromaBytes * 2);
			std::memcpy(copiedI420.data(), yPlane.constData(), nYBytes);
			std::memcpy(copiedI420.data() + nYBytes,
				uPlane.constData(), nChromaBytes);
			std::memcpy(copiedI420.data() + nYBytes + nChromaBytes,
				vPlane.constData(), nChromaBytes);
			nChecksum += static_cast<unsigned char>(copiedI420.at(nFrame % nYBytes));
		}

		const qint64 nWallUs = timer.nsecsElapsed() / 1000;
		const KProcessSample after = ProcessSample();
		QJsonObject result = CommonResult(QStringLiteral("legacy"), nWidth, nHeight,
			nFrames, nWallUs, after.nCpu100Ns - before.nCpu100Ns,
			after.nPeakWorkingSetBytes);
		result.insert(QStringLiteral("swsContextCreations"), nFrames);
		result.insert(QStringLiteral("pixelBufferAllocations"), nFrames * 4);
		result.insert(QStringLiteral("copyBytes"),
			static_cast<double>(nCopiedBytesPerFrame * static_cast<quint64>(nFrames)));
		result.insert(QStringLiteral("checksum"), static_cast<double>(nChecksum));
		return result;
	}

	QJsonObject RunOptimized(KCaptureFrame *pFrame, int nFrames)
	{
		KCaptureFrameSink sink(KCaptureFrameSink::RemoteVideoSinkMode);
		QString strError;
		if (!sink.initialize(&strError))
			return {};

		KVideoFrame latestFrame;
		QList<quintptr> uniqueBuffers;
		int nDeliveredFrames = 0;
		QObject::connect(&sink, &KCaptureFrameSink::videoFrameReady,
			[&](const KVideoFrame &frame)
			{
				latestFrame = frame;
				const quintptr nAddress = reinterpret_cast<quintptr>(frame.spBuffer.get());
				if (!uniqueBuffers.contains(nAddress))
					uniqueBuffers.append(nAddress);
				++nDeliveredFrames;
			});

		const KProcessSample before = ProcessSample();
		QElapsedTimer timer;
		timer.start();
		for (int nFrame = 0; nFrame < nFrames; ++nFrame)
		{
			pFrame->nFrameIndex = static_cast<quint64>(nFrame + 1);
			if (!sink.processFrame(*pFrame, &strError))
				return {};
		}
		const qint64 nWallUs = timer.nsecsElapsed() / 1000;
		const KProcessSample after = ProcessSample();
		sink.shutdown();

		QJsonObject result = CommonResult(QStringLiteral("optimized"),
			pFrame->nWidth & ~1, pFrame->nHeight & ~1, nFrames, nWallUs,
			after.nCpu100Ns - before.nCpu100Ns, after.nPeakWorkingSetBytes);
		result.insert(QStringLiteral("swsContextCreations"), 1);
		result.insert(QStringLiteral("pixelBufferAllocations"), uniqueBuffers.size() * 3);
		result.insert(QStringLiteral("copyBytes"), 0);
		result.insert(QStringLiteral("uniqueFrameBuffers"), uniqueBuffers.size());
		result.insert(QStringLiteral("deliveredFrames"), nDeliveredFrames);
		result.insert(QStringLiteral("droppedFrames"), nFrames - nDeliveredFrames);
		return result;
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	const QStringList arguments = application.arguments();
	const int nModeIndex = arguments.indexOf(QStringLiteral("--mode"));
	if (nModeIndex < 0 || nModeIndex + 1 >= arguments.size())
	{
		std::cerr << "Usage: wrc_video_pipeline_benchmark --mode legacy|optimized "
			"[--width N] [--height N] [--frames N]\n";
		return 2;
	}

	int nWidth = 0;
	int nHeight = 0;
	int nFrames = 0;
	if (!ReadPositiveArgument(arguments, QStringLiteral("--width"), 1280, &nWidth)
		|| !ReadPositiveArgument(arguments, QStringLiteral("--height"), 720, &nHeight)
		|| !ReadPositiveArgument(arguments, QStringLiteral("--frames"), 120, &nFrames)
		|| nWidth < 2 || nHeight < 2)
	{
		std::cerr << "Invalid benchmark dimensions or frame count\n";
		return 2;
	}

	KCaptureFrame frame;
	frame.nWidth = nWidth;
	frame.nHeight = nHeight;
	frame.vecBgraBuffer.resize(static_cast<size_t>(nWidth) * nHeight * 4);
	for (size_t nIndex = 0; nIndex < frame.vecBgraBuffer.size(); ++nIndex)
		frame.vecBgraBuffer[nIndex] = static_cast<unsigned char>((nIndex * 31U) & 0xffU);

	const QString strMode = arguments.at(nModeIndex + 1);
	const QJsonObject result = strMode == QStringLiteral("legacy")
		? RunLegacy(&frame, nFrames)
		: strMode == QStringLiteral("optimized")
			? RunOptimized(&frame, nFrames)
			: QJsonObject();
	if (result.isEmpty())
	{
		std::cerr << "Video pipeline benchmark failed\n";
		return 1;
	}
	std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
	return 0;
}
