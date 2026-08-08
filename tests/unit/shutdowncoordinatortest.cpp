#include "session/shutdowncoordinator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const char *pDescription)
	{
		if (bCondition)
			return;
		std::cerr << "FAILED: " << pDescription << '\n';
		++g_nFailureCount;
	}

	void ProcessEventsFor(int nMilliseconds)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < nMilliseconds)
		{
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	KShutdownCoordinator coordinator;
	quint64 nFinishedGeneration = 0;
	quint64 nWatchdogGeneration = 0;
	bool bWatchdogCapturePending = false;
	bool bWatchdogPeerPending = false;
	QObject::connect(&coordinator, &KShutdownCoordinator::finished,
		[&](quint64 nGeneration) { nFinishedGeneration = nGeneration; });
	QObject::connect(&coordinator, &KShutdownCoordinator::watchdogExpired,
		[&](quint64 nGeneration, bool bCapturePending, bool bPeerPending)
		{
			nWatchdogGeneration = nGeneration;
			bWatchdogCapturePending = bCapturePending;
			bWatchdogPeerPending = bPeerPending;
		});

	coordinator.begin(7, true, true, 1000);
	coordinator.completeCapture(6);
	Check(coordinator.isCapturePending(),
		"an old generation cannot complete capture shutdown");
	coordinator.completeCapture(7);
	Check(!coordinator.isCapturePending() && coordinator.isPeerPending(),
		"capture and peer shutdown complete independently");
	coordinator.completePeer(7);
	Check(nFinishedGeneration == 7 && !coordinator.isActive(),
		"shutdown finishes once all components complete");

	coordinator.begin(8, true, false, 10);
	ProcessEventsFor(20);
	Check(nWatchdogGeneration == 8
		&& bWatchdogCapturePending && !bWatchdogPeerPending,
		"watchdog reports the exact pending components");
	coordinator.clear();

	return g_nFailureCount == 0 ? 0 : 1;
}
