#include "session/accessapprovalcontroller.h"

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
	KAccessApprovalController controller;
	QString strTimedOutRequestId;
	quint64 nTimedOutGeneration = 0;
	QObject::connect(&controller, &KAccessApprovalController::timedOut,
		[&](const QString &strRequestId, quint64 nGeneration)
		{
			strTimedOutRequestId = strRequestId;
			nTimedOutGeneration = nGeneration;
		});

	const KAccessApprovalRequest outgoing = controller.beginOutgoing(7, 10);
	Check(!outgoing.strRequestId.isEmpty()
		&& outgoing.side == OutgoingAccessApprovalSide
		&& controller.matches(outgoing.strRequestId, 7),
		"outgoing approval owns its request and generation");
	Check(!controller.extendOutgoingTimeout(QStringLiteral("unknown"), 10),
		"an unrelated request cannot extend the approval timeout");
	ProcessEventsFor(20);
	Check(strTimedOutRequestId == outgoing.strRequestId && nTimedOutGeneration == 7,
		"approval timeout preserves request and generation");

	controller.beginIncoming(QStringLiteral("192.0.2.1"), 8, 1000);
	Check(controller.receiveIncomingRequest(
		QStringLiteral("12345678-1234-1234-1234-1234567890ab"),
		QStringLiteral("test-device"), 1000),
		"incoming approval accepts the first access request");
	Check(!controller.receiveIncomingRequest(
		QStringLiteral("22345678-1234-1234-1234-1234567890ab"),
		QStringLiteral("other-device"), 1000),
		"incoming approval rejects a second request in the occupied slot");
	const KAccessApprovalRequest incoming = controller.clear();
	Check(incoming.side == IncomingAccessApprovalSide
		&& incoming.strSourceAddress == QStringLiteral("192.0.2.1")
		&& !controller.hasRequestId(),
		"clearing approval returns prior context and releases the slot");

	KApplicationSettings settings;
	Check(KAccessApprovalController::incomingDecision(settings)
		== AskIncomingAccessDecision,
		"ask is the safe default approval decision");
	settings.approvalMode = AutoAcceptRemoteApprovalMode;
	Check(KAccessApprovalController::incomingDecision(settings)
		== AcceptIncomingAccessDecision,
		"auto-accept setting is mapped explicitly");
	settings.bRemoteAccessEnabled = false;
	Check(KAccessApprovalController::incomingDecision(settings)
		== DisabledIncomingAccessDecision,
		"disabled remote access overrides approval mode");

	return g_nFailureCount == 0 ? 0 : 1;
}
