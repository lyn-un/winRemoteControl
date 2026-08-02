#include "app/mainwindow.h"

#include "core/media/decodedvideoframe.h"
#include "common/latencytracelogger.h"
#include "common/sessiontracelogger.h"
#include "common/traceoptions.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/clipboardmessage.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QMetaType>
#include <QtNetwork/QNetworkProxy>
#include <QtWidgets/QApplication>

#include <objbase.h>

int main(int nArgc, char *pArgv[])
{
	const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hrCom))
		return -1;

	QApplication app(nArgc, pArgv);
	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("winRemoteControl LAN remote desktop client"));
	parser.addHelpOption();
	KTraceOptionsParser::addOptions(&parser);
	parser.process(app);
	const KTraceOptions traceOptions = KTraceOptionsParser::options(
		parser, QCoreApplication::applicationDirPath());
	KSessionTraceLogger::configure(
		traceOptions.bSessionTraceEnabled, traceOptions.strLogDirectory);
	KLatencyTraceLogger::configure(
		traceOptions.bLatencyTraceEnabled, traceOptions.strLogDirectory);
	KLatencyTraceLogger::write(QStringLiteral("app"),
		QStringLiteral("startup"),
		QStringLiteral("dir=%1").arg(QCoreApplication::applicationDirPath()));
	QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
	qRegisterMetaType<KDecodedVideoFrame>("KDecodedVideoFrame");
	qRegisterMetaType<KStreamConfig>("KStreamConfig");
	qRegisterMetaType<KNetworkStats>("KNetworkStats");
	qRegisterMetaType<KVideoFrame>("KVideoFrame");
	qRegisterMetaType<KClipboardMessage>("KClipboardMessage");

	KMainWindow mainWindow;
	mainWindow.resize(1280, 760);
	mainWindow.show();

	const int nResult = app.exec();
	KLatencyTraceLogger::write(QStringLiteral("app"), QStringLiteral("shutdown"));
	KLatencyTraceLogger::shutdown();
	::CoUninitialize();
	return nResult;
}
