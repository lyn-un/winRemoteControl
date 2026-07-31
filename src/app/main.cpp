#include "app/mainwindow.h"

#include "core/media/decodedvideoframe.h"
#include "common/latencytracelogger.h"
#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/media/videoframe.h"

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
	KLatencyTraceLogger::write(QStringLiteral("app"),
		QStringLiteral("startup"),
		QStringLiteral("dir=%1").arg(QCoreApplication::applicationDirPath()));
	QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
	qRegisterMetaType<KDecodedVideoFrame>("KDecodedVideoFrame");
	qRegisterMetaType<KStreamConfig>("KStreamConfig");
	qRegisterMetaType<KNetworkStats>("KNetworkStats");
	qRegisterMetaType<KVideoFrame>("KVideoFrame");

	KMainWindow mainWindow;
	mainWindow.resize(1280, 760);
	mainWindow.show();

	const int nResult = app.exec();
	KLatencyTraceLogger::write(QStringLiteral("app"), QStringLiteral("shutdown"));
	KLatencyTraceLogger::shutdown();
	::CoUninitialize();
	return nResult;
}
