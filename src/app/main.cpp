#include "app/mainwindow.h"

#include "core/media/decodedvideoframe.h"
#include "common/latencytracelogger.h"
#include "common/applicationpaths.h"
#include "common/sessiontracelogger.h"
#include "common/traceoptions.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"
#include "core/protocol/clipboardmessage.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QDebug>
#include <QtCore/QMetaType>
#include <QtGui/QIcon>
#include <QtNetwork/QNetworkProxy>
#include <QtWidgets/QApplication>

#include <objbase.h>

int main(int nArgc, char *pArgv[])
{
	const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hrCom))
		return -1;

	QApplication app(nArgc, pArgv);
	app.setWindowIcon(QIcon(QStringLiteral(":/branding/app-icon.png")));
	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("winRemoteControl LAN remote desktop client"));
	parser.addHelpOption();
	parser.addOption(QCommandLineOption(QStringLiteral("data-dir"),
		QStringLiteral("Store application data in the specified directory."),
		QStringLiteral("path")));
	parser.addOption(QCommandLineOption(QStringLiteral("automation-test-profile"),
		QStringLiteral("Delete the temporary CNG identity when this test process exits.")));
	KTraceOptionsParser::addOptions(&parser);
	parser.process(app);
	QString strPathError;
	if (!KApplicationPaths::configureDataDirectory(
		parser.value(QStringLiteral("data-dir")), &strPathError))
	{
		qCritical().noquote() << strPathError;
		::CoUninitialize();
		return 2;
	}
	KApplicationPaths::setAutomationTestProfile(
		parser.isSet(QStringLiteral("automation-test-profile")));
	const KTraceOptions traceOptions = KTraceOptionsParser::options(
		parser, KApplicationPaths::dataDirectoryPath());
	if (!traceOptions.strValidationError.isEmpty())
	{
		qCritical().noquote() << traceOptions.strValidationError;
		::CoUninitialize();
		return 2;
	}
	KSessionTraceLogger::configure(
		traceOptions.bSessionTraceEnabled, traceOptions.strLogDirectory);
	KLatencyTraceLogger::configure(
		traceOptions.bLatencyTraceEnabled,
		traceOptions.strLogDirectory,
		traceOptions.strLatencyScenario);
	KLatencyTraceLogger::write(QStringLiteral("app"),
		QStringLiteral("startup"),
		QStringLiteral("dir=%1 dataDir=%2")
			.arg(QCoreApplication::applicationDirPath(), KApplicationPaths::dataDirectoryPath()));
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
