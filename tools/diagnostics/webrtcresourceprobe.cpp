#include "transport/webrtc/webrtcpeer.h"
#include "tools/diagnostics/resourceprobecommon.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QThread>
#include <QtCore/QTimer>

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	QCommandLineParser parser;
	parser.addHelpOption();
	parser.addOption(QCommandLineOption(QStringLiteral("cycles"),
		QStringLiteral("Number of WebRTC peer create/destroy cycles."),
		QStringLiteral("count"), QStringLiteral("5")));
	parser.process(application);
	bool bCyclesValid = false;
	const int nCycles = parser.value(QStringLiteral("cycles")).toInt(&bCyclesValid);
	if (!bCyclesValid || nCycles < 1 || nCycles > 100)
		return 2;

	KWebRtcPeer peer;
	PrintResourceProbeSnapshot(QStringLiteral("webrtc"), 0, QStringLiteral("baseline"));
	for (int nCycle = 1; nCycle <= nCycles; ++nCycle)
	{
		const KPeerInitializationResult result = peer.initialize(
			ControlledSessionRole, static_cast<quint64>(nCycle));
		if (!result.succeeded())
		{
			qCritical().noquote() << result.strTechnicalMessage;
			return 3;
		}

		QEventLoop waitLoop;
		QTimer timeout;
		timeout.setSingleShot(true);
		QObject::connect(&timeout, &QTimer::timeout, &waitLoop, &QEventLoop::quit);
		QObject::connect(&peer, &KRemotePeerTransport::shutdownFinished,
			&waitLoop, &QEventLoop::quit);
		timeout.start(10000);
		peer.requestShutdown(static_cast<quint64>(nCycle));
		waitLoop.exec();
		if (!timeout.isActive())
			return 4;
		timeout.stop();
		QThread::msleep(500);
		PrintResourceProbeSnapshot(QStringLiteral("webrtc"),
			nCycle, QStringLiteral("shutdown"));
	}
	return 0;
}
