#include "session/sessioncommanddispatcher.h"

#include <QtCore/QCoreApplication>

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
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	KSessionCommandDispatcher dispatcher;
	QList<KSessionMessage> transmittedMessages;
	dispatcher.setTransmitFunction(
		[&](const KSessionMessage &message)
		{
			transmittedMessages.append(message);
			return true;
		});

	int nHandledCount = 0;
	dispatcher.registerHandler(StartStreamingSessionMessageType,
		[&](const KSessionMessage &)
		{
			++nHandledCount;
			return KProtocolHandlerResult::success();
		});

	KSessionMessage incoming;
	incoming.type = StartStreamingSessionMessageType;
	incoming.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
	const KSessionIncomingDispatchResult first = dispatcher.handleIncoming(incoming, 7);
	const KSessionIncomingDispatchResult duplicate = dispatcher.handleIncoming(incoming, 7);
	Check(first.handlerResult.status == ProtocolHandlerSucceeded
		&& duplicate.handlerResult.status == ProtocolHandlerSucceeded
		&& nHandledCount == 1,
		"duplicate commands reuse the cached result without executing twice");
	Check(transmittedMessages.size() == 2
		&& transmittedMessages.at(0).type == CommandResultSessionMessageType
		&& transmittedMessages.at(1).strRequestId == incoming.strRequestId,
		"each command delivery receives an acknowledgement");

	bool bCompleted = false;
	QObject::connect(&dispatcher, &KSessionCommandDispatcher::commandCompleted,
		[&](KSessionMessageType type, const QString &, bool bSuccess,
			const QString &, quint64 nGeneration)
		{
			bCompleted = type == StopStreamingSessionMessageType
				&& bSuccess && nGeneration == 9;
		});
	KSessionMessage outgoing;
	outgoing.type = StopStreamingSessionMessageType;
	const QString strRequestId = dispatcher.send(outgoing, 9);
	Check(!strRequestId.isEmpty()
		&& transmittedMessages.last().strRequestId == strRequestId,
		"outgoing commands receive and transmit a request id");
	KSessionMessage result;
	result.type = CommandResultSessionMessageType;
	result.strRequestId = strRequestId;
	result.bSuccess = true;
	dispatcher.handleIncoming(result, 9);
	Check(bCompleted, "a matching result completes the pending command");

	return g_nFailureCount == 0 ? 0 : 1;
}
