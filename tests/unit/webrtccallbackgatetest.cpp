#include "transport/webrtc/webrtccallbackgate.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QThread>

#include <iostream>
#include <memory>
#include <thread>

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
	QObject target;
	const QThread *pOwnerThread = target.thread();
	auto spGate = std::make_shared<KWebRtcCallbackGate>();
	spGate->open(&target, 7);

	bool bCalled = false;
	bool bOwnerThread = false;
	std::thread callbackThread(
		[spGate, &bCalled, &bOwnerThread, pOwnerThread]()
		{
			spGate->post(7,
				[&bCalled, &bOwnerThread, pOwnerThread](QObject *)
				{
					bCalled = true;
					bOwnerThread = QThread::currentThread() == pOwnerThread;
				});
		});
	callbackThread.join();
	ProcessEventsFor(20);
	Check(bCalled && bOwnerThread,
		"a foreign callback executes on the target Qt thread");

	bCalled = false;
	spGate->post(6, [&bCalled](QObject *) { bCalled = true; });
	ProcessEventsFor(10);
	Check(!bCalled, "an old generation is rejected");

	spGate->post(7, [&bCalled](QObject *) { bCalled = true; });
	spGate->close();
	ProcessEventsFor(10);
	Check(!bCalled, "closing the gate cancels queued callbacks");

	return g_nFailureCount == 0 ? 0 : 1;
}
