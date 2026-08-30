#include "automation/driver/app/wrcdrivermodule.h"
#include "automation/wrcdriverhostapi.h"

#include <QtCore/QMetaObject>
#include <QtCore/QMutexLocker>
#include <QtCore/QDebug>
#include <QtCore/QThread>
#include <QtCore/QtGlobal>

#include <cstring>

namespace
{
	QThread *g_pDriverThread = nullptr;
	KWrcDriverModule *g_pDriverModule = nullptr;
	KWrcDriverCallbackGate g_callbackGate;

	std::uint32_t RuntimeFlavor()
	{
#ifdef QT_DEBUG
		return KWrcDriverRuntimeDebug;
#else
		return KWrcDriverRuntimeRelease;
#endif
	}
}

extern "C" __declspec(dllexport) std::uint32_t wrcDriverAbiVersion()
{
	return KWrcDriverAbiVersion2;
}

extern "C" __declspec(dllexport) bool wrcDriverBuildInfo(KWrcDriverBuildInfoV2 *pBuildInfo)
{
	if (pBuildInfo == nullptr || pBuildInfo->nStructSize < sizeof(KWrcDriverBuildInfoV2))
		return false;
	pBuildInfo->nAbiVersion = KWrcDriverAbiVersion2;
	pBuildInfo->nArchitecture = KWrcDriverArchitectureX64;
	pBuildInfo->nRuntimeFlavor = RuntimeFlavor();
	pBuildInfo->nQtMajorVersion = QT_VERSION_MAJOR;
	std::memset(pBuildInfo->szBuildId, 0, sizeof(pBuildInfo->szBuildId));
	std::memcpy(pBuildInfo->szBuildId, KWrcAutomationBuildId,
		sizeof(KWrcAutomationBuildId));
	return true;
}

extern "C" __declspec(dllexport) bool wrcDriverStartup(const KWrcDriverHostApiV2 *pHostApi)
{
	if (g_pDriverThread != nullptr || g_pDriverModule != nullptr)
		return false;
	g_pDriverThread = new QThread();
	g_pDriverModule = new KWrcDriverModule();
	{
		QMutexLocker locker(&g_callbackGate.mutex);
		g_callbackGate.pModule = g_pDriverModule;
		g_callbackGate.startedRequestIds.clear();
	}
	g_pDriverModule->moveToThread(g_pDriverThread);
	QObject::connect(g_pDriverThread, &QThread::finished,
		g_pDriverModule, &QObject::deleteLater);
	g_pDriverThread->start();
	bool bStarted = false;
	QString strError;
	QMetaObject::invokeMethod(g_pDriverModule,
		[&bStarted, &strError, pHostApi]()
		{
			bStarted = g_pDriverModule->start(pHostApi, &g_callbackGate, &strError);
		}, Qt::BlockingQueuedConnection);
	if (bStarted)
		return true;
	{
		QMutexLocker locker(&g_callbackGate.mutex);
		g_callbackGate.pModule = nullptr;
		g_callbackGate.startedRequestIds.clear();
	}
	g_pDriverThread->quit();
	g_pDriverThread->wait();
	delete g_pDriverThread;
	g_pDriverThread = nullptr;
	g_pDriverModule = nullptr;
	return false;
}

extern "C" __declspec(dllexport) void wrcDriverShutdown()
{
	if (g_pDriverThread == nullptr || g_pDriverModule == nullptr)
		return;
	{
		QMutexLocker locker(&g_callbackGate.mutex);
		g_callbackGate.pModule = nullptr;
		g_callbackGate.startedRequestIds.clear();
	}
	QMetaObject::invokeMethod(g_pDriverModule,
		[]() { g_pDriverModule->stop(); }, Qt::BlockingQueuedConnection);
	g_pDriverThread->quit();
	if (!g_pDriverThread->wait(5000))
	{
		g_pDriverThread->requestInterruption();
		g_pDriverThread->quit();
		if (!g_pDriverThread->wait(1000))
		{
			qCritical("Automation driver thread did not stop within 6000 ms; DLL remains loaded");
			return;
		}
	}
	delete g_pDriverThread;
	g_pDriverThread = nullptr;
	g_pDriverModule = nullptr;
}
