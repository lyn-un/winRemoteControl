#include "terminal/terminalcommanddispatcher.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QUuid>

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
		QThread::msleep(10);
	}
}

KTerminalMessage OpenCommand()
{
	KTerminalMessage message;
	message.type = OpenRequestTerminalMessageType;
	message.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	message.strCommandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	message.nColumns = 100;
	message.nRows = 30;
	return message;
}

KTerminalMessage ResizeCommand(const QString &strRequestId, int nColumns)
{
	KTerminalMessage message;
	message.type = ResizeTerminalMessageType;
	message.strRequestId = strRequestId;
	message.nColumns = nColumns;
	message.nRows = 30;
	return message;
}

void TestDuplicateCommandIsIdempotent()
{
	KTerminalCommandDispatcher dispatcher;
	QVector<KTerminalMessage> transmitted;
	int nHandleCount = 0;
	dispatcher.setTransmitFunction([&transmitted](const KTerminalMessage &message)
		{
			transmitted.append(message);
			return true;
		});
	dispatcher.setHandler([&nHandleCount](const KTerminalMessage &, QString *)
		{
			++nHandleCount;
			return true;
		});
	const KTerminalMessage command = OpenCommand();
	dispatcher.handleIncoming(command, 7);
	dispatcher.handleIncoming(command, 7);
	Check(nHandleCount == 1, "duplicate command executes once");
	Check(transmitted.size() == 2
		&& transmitted.at(0).type == CommandResultTerminalMessageType
		&& transmitted.at(1).strCommandId == transmitted.at(0).strCommandId,
		"duplicate command returns cached result");
}

void TestAckLossRetriesThenTimesOut()
{
	KTerminalCommandDispatcher dispatcher;
	int nTransmitCount = 0;
	int nTimeoutCount = 0;
	dispatcher.setTransmitFunction([&nTransmitCount](const KTerminalMessage &)
		{
			++nTransmitCount;
			return true;
		});
	QObject::connect(&dispatcher, &KTerminalCommandDispatcher::commandTimedOut,
		[&nTimeoutCount](KTerminalMessageType, const QString &, const QString &, quint64)
			{ ++nTimeoutCount; });
	Check(dispatcher.send(OpenCommand(), 9), "reliable command is accepted");
	ProcessEventsFor(2300);
	Check(nTransmitCount == 2, "lost ACK causes one bounded retry");
	Check(nTimeoutCount == 1, "lost ACK produces one timeout");
}

void TestAckCompletesWithoutRetry()
{
	KTerminalCommandDispatcher dispatcher;
	QVector<KTerminalMessage> transmitted;
	int nCompletedCount = 0;
	dispatcher.setTransmitFunction([&transmitted](const KTerminalMessage &message)
		{
			transmitted.append(message);
			return true;
		});
	QObject::connect(&dispatcher, &KTerminalCommandDispatcher::commandCompleted,
		[&nCompletedCount](KTerminalMessageType, const QString &, const QString &,
			bool bSuccess, const QString &, quint64)
			{ nCompletedCount += bSuccess ? 1 : 0; });
	const KTerminalMessage command = OpenCommand();
	Check(dispatcher.send(command, 11), "command with ACK is accepted");
	KTerminalMessage result;
	result.type = CommandResultTerminalMessageType;
	result.strRequestId = command.strRequestId;
	result.strCommandId = command.strCommandId;
	result.bSuccess = true;
	dispatcher.handleIncoming(result, 11);
	ProcessEventsFor(1200);
	Check(nCompletedCount == 1, "matching ACK completes command");
	Check(transmitted.size() == 1, "completed command is not retried");
}

void TestSynchronousAckIsNotLost()
{
	KTerminalCommandDispatcher dispatcher;
	int nCompletedCount = 0;
	dispatcher.setTransmitFunction([&dispatcher](const KTerminalMessage &message)
		{
			KTerminalMessage result;
			result.type = CommandResultTerminalMessageType;
			result.strRequestId = message.strRequestId;
			result.strCommandId = message.strCommandId;
			result.bSuccess = true;
			dispatcher.handleIncoming(result, 13);
			return true;
		});
	QObject::connect(&dispatcher, &KTerminalCommandDispatcher::commandCompleted,
		[&nCompletedCount](KTerminalMessageType, const QString &, const QString &,
			bool, const QString &, quint64) { ++nCompletedCount; });
	Check(dispatcher.send(OpenCommand(), 13), "synchronous ACK command is accepted");
	Check(nCompletedCount == 1, "synchronous ACK is observed exactly once");
}

void TestResizeCoalescesToLatestPendingSize()
{
	KTerminalCommandDispatcher dispatcher;
	QVector<KTerminalMessage> transmitted;
	dispatcher.setTransmitFunction([&transmitted](const KTerminalMessage &message)
		{
			transmitted.append(message);
			return true;
		});
	const QString strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	Check(dispatcher.send(ResizeCommand(strRequestId, 100), 15),
		"first resize is sent");
	Check(dispatcher.send(ResizeCommand(strRequestId, 120), 15)
		&& dispatcher.send(ResizeCommand(strRequestId, 140), 15),
		"later resizes are accepted while one is pending");
	Check(transmitted.size() == 1 && transmitted.first().nColumns == 100,
		"only one resize remains in flight");
	KTerminalMessage result;
	result.type = CommandResultTerminalMessageType;
	result.strRequestId = strRequestId;
	result.strCommandId = transmitted.first().strCommandId;
	result.bSuccess = true;
	dispatcher.handleIncoming(result, 15);
	ProcessEventsFor(20);
	Check(transmitted.size() == 2 && transmitted.last().nColumns == 140,
		"completion sends only the latest deferred resize");
}

void TestPendingCommandTableIsBounded()
{
	KTerminalCommandDispatcher dispatcher;
	dispatcher.setTransmitFunction([](const KTerminalMessage &) { return true; });
	for (int index = 0; index < 128; ++index)
		Check(dispatcher.send(OpenCommand(), 17), "bounded command slot is accepted");
	Check(!dispatcher.send(OpenCommand(), 17),
		"pending reliable command table rejects overflow");
}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestDuplicateCommandIsIdempotent();
	TestAckLossRetriesThenTimesOut();
	TestAckCompletesWithoutRetry();
	TestSynchronousAckIsNotLost();
	TestResizeCoalescesToLatestPendingSize();
	TestPendingCommandTableIsBounded();
	return g_nFailureCount == 0 ? 0 : 1;
}
