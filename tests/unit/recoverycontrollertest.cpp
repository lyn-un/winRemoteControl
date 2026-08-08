#include "session/recoverycontroller.h"

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
	KRecoveryController controller;
	quint64 nTimedOutGeneration = 0;
	QObject::connect(&controller, &KRecoveryController::timedOut,
		[&](quint64 nGeneration) { nTimedOutGeneration = nGeneration; });

	controller.begin(11, 1000);
	ProcessEventsFor(5);
	Check(controller.isActive(11) && !controller.isActive(10),
		"recovery is isolated by generation");
	Check(controller.complete(10) == -1 && controller.isActive(11),
		"an old generation cannot complete recovery");
	Check(controller.complete(11) >= 0 && !controller.isActive(11),
		"matching recovery completion returns elapsed time and clears state");

	controller.begin(12, 10);
	ProcessEventsFor(20);
	Check(nTimedOutGeneration == 12 && controller.isActive(12),
		"timeout preserves its generation until the owner handles it");
	controller.clear();

	return g_nFailureCount == 0 ? 0 : 1;
}
