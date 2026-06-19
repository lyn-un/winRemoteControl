#include "app/mainwindow.h"

#include "codec/decodedvideoframe.h"
#include "common/latencytracelogger.h"
#include "common/streamconfig.h"
#include "transport/webrtc/webrtcnetworkstats.h"
#include "transport/webrtc/webrtcvideoframe.h"

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
	qRegisterMetaType<KWebRtcNetworkStats>("KWebRtcNetworkStats");
	qRegisterMetaType<KWebRtcVideoFrame>("KWebRtcVideoFrame");

	KMainWindow mainWindow;
	mainWindow.resize(1280, 760);
	mainWindow.show();

	const int nResult = app.exec();
	::CoUninitialize();
	return nResult;
}
