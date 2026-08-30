#include "codec/h264encoder.h"
#include "tools/diagnostics/resourceprobecommon.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QThread>

#include <vector>

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
	parser.process(application);

	bool bCyclesValid = false;
	const int nCycles = parser.value(QStringLiteral("cycles")).toInt(&bCyclesValid);
	const QString strEncoder = parser.value(QStringLiteral("encoder")).trimmed().toLower();
	if (!bCyclesValid || nCycles < 1 || nCycles > 100
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
