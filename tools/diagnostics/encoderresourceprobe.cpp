#include "codec/h264encoder.h"
#include "tools/diagnostics/resourceprobecommon.h"
#include "transport/webrtc/webrtch264encoder.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QThread>

#include <api/video/video_bitrate_allocation.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <iterator>
#include <vector>

namespace
{
	webrtc::VideoEncoder::RateControlParameters MakeRates(int nFps, int nBitrateKbps)
	{
		webrtc::VideoBitrateAllocation allocation;
		allocation.SetBitrate(0, 0, static_cast<uint32_t>(nBitrateKbps * 1000));
		return webrtc::VideoEncoder::RateControlParameters(allocation,
			static_cast<double>(nFps));
	}

	int RunRateChangeProbe(KVideoEncoderPreference preference,
		const QString &strEncoder,
		int nRateChanges)
	{
		KWebRtcH264Encoder encoder(preference);
		webrtc::VideoCodec codec = {};
		codec.width = 1280;
		codec.height = 720;
		codec.maxFramerate = 60;
		codec.startBitrate = 4000;
		PrintResourceProbeSnapshot(strEncoder, 0, QStringLiteral("rate_baseline"));
		const webrtc::VideoEncoder::Settings settings(
			webrtc::VideoEncoder::Capabilities(false), 1, 1200);
		if (encoder.InitEncode(&codec, settings)
			!= WEBRTC_VIDEO_CODEC_OK)
		{
			return 5;
		}

		constexpr int kFpsChoices[] = { 27, 14, 15, 7, 1, 30, 60 };
		for (int nIndex = 0; nIndex < nRateChanges; ++nIndex)
		{
			encoder.SetRates(MakeRates(kFpsChoices[nIndex % std::size(kFpsChoices)],
				3000 + (nIndex % 1000)));
			if ((nIndex + 1) % 25 == 0 || nIndex + 1 == nRateChanges)
			{
				PrintResourceProbeSnapshot(strEncoder,
					nIndex + 1,
					QStringLiteral("rate_changed"));
			}
		}

		encoder.Release();
		QThread::msleep(500);
		PrintResourceProbeSnapshot(strEncoder, nRateChanges, QStringLiteral("rate_closed"));
		return 0;
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QCommandLineParser parser;
	parser.addHelpOption();
	parser.addOption(QCommandLineOption(QStringLiteral("encoder"),
		QStringLiteral("Encoder to test: auto, h264_mf, or libx264."),
		QStringLiteral("name"), QStringLiteral("h264_mf")));
	parser.addOption(QCommandLineOption(QStringLiteral("cycles"),
		QStringLiteral("Number of encoder create/destroy cycles."),
		QStringLiteral("count"), QStringLiteral("5")));
	parser.addOption(QCommandLineOption(QStringLiteral("rate-changes"),
		QStringLiteral("Keep one encoder open while applying this many WebRTC rate changes."),
		QStringLiteral("count"), QStringLiteral("0")));
	parser.process(application);

	bool bCyclesValid = false;
	const int nCycles = parser.value(QStringLiteral("cycles")).toInt(&bCyclesValid);
	bool bRateChangesValid = false;
	const int nRateChanges = parser.value(QStringLiteral("rate-changes")).toInt(&bRateChangesValid);
	const QString strEncoder = parser.value(QStringLiteral("encoder")).trimmed().toLower();
	if (!bCyclesValid || nCycles < 1 || nCycles > 100
		|| !bRateChangesValid || nRateChanges < 0 || nRateChanges > 10000
		|| (strEncoder != QStringLiteral("auto")
			&& strEncoder != QStringLiteral("h264_mf")
			&& strEncoder != QStringLiteral("libx264")))
	{
		return 2;
	}

	KVideoEncoderPreference preference = AutoVideoEncoderPreference;
	if (strEncoder == QStringLiteral("h264_mf"))
		preference = MediaFoundationVideoEncoderPreference;
	else if (strEncoder == QStringLiteral("libx264"))
		preference = LibX264VideoEncoderPreference;
	if (nRateChanges > 0)
		return RunRateChangeProbe(preference, strEncoder, nRateChanges);

	constexpr int kWidth = 1280;
	constexpr int kHeight = 720;
	std::vector<unsigned char> vecFrame(kWidth * kHeight * 4, 0x20);
	PrintResourceProbeSnapshot(strEncoder, 0, QStringLiteral("baseline"));
	for (int nCycle = 1; nCycle <= nCycles; ++nCycle)
	{
		KH264Encoder encoder(preference);
		QString strError;
		if (!encoder.openStream(kWidth, kHeight, 30, 4000,
			[](const QByteArray &) {}, &strError))
		{
			qCritical().noquote() << strError;
			return 3;
		}
		for (int nFrame = 0; nFrame < 60; ++nFrame)
		{
			if (!encoder.encodeBgraFrame(vecFrame.data(), kWidth, kHeight,
				nFrame * 33, &strError))
			{
				qCritical().noquote() << strError;
				return 4;
			}
		}
		if (!encoder.close(&strError))
			qWarning().noquote() << strError;
		QThread::msleep(500);
		PrintResourceProbeSnapshot(strEncoder, nCycle, QStringLiteral("closed"));
	}
	return 0;
}
