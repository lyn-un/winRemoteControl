#include "session/sessioncommanddispatcher.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>

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
			return KSessionCommandTransmitResult { true, QString() };
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

	const int nMessagesBeforeRetry = transmittedMessages.size();
	KSessionMessage retryCommand;
	retryCommand.type = StartStreamingSessionMessageType;
	const QString strRetryRequestId = dispatcher.send(retryCommand, 10);
	QEventLoop retryWait;
	QTimer::singleShot(1300, &retryWait, &QEventLoop::quit);
	retryWait.exec();
	Check(transmittedMessages.size() == nMessagesBeforeRetry + 2
		&& transmittedMessages.at(nMessagesBeforeRetry).strRequestId == strRetryRequestId
		&& transmittedMessages.at(nMessagesBeforeRetry + 1).strRequestId == strRetryRequestId,
		"a missing result retries the same request id");
	result.strRequestId = strRetryRequestId;
	dispatcher.handleIncoming(result, 10);

	int nGenerationCompletionCount = 0;
	QString strGenerationRequestId;
	QObject::connect(&dispatcher, &KSessionCommandDispatcher::commandCompleted,
		[&](KSessionMessageType, const QString &strCompletedRequestId, bool,
			const QString &, quint64)
		{
			if (strCompletedRequestId == strGenerationRequestId)
				++nGenerationCompletionCount;
		});
	KSessionMessage generationCommand;
	generationCommand.type = StreamConfigSessionMessageType;
	strGenerationRequestId = dispatcher.send(generationCommand, 11);
	result.strRequestId = strGenerationRequestId;
	dispatcher.handleIncoming(result, 10);
	Check(nGenerationCompletionCount == 0,
		"an acknowledgement from an old generation is ignored");
	dispatcher.handleIncoming(result, 11);
	Check(nGenerationCompletionCount == 1,
		"the matching generation completes the command");

	QString strFailedRequestId;
	QString strFailureCode;
	QObject::connect(&dispatcher, &KSessionCommandDispatcher::commandCompleted,
		[&](KSessionMessageType, const QString &strCompletedRequestId, bool bSuccess,
			const QString &strErrorCode, quint64)
		{
			if (strCompletedRequestId != strFailedRequestId || bSuccess)
				return;
			strFailureCode = strErrorCode;
		});
	KSessionMessage pendingCommand;
	pendingCommand.type = StopStreamingSessionMessageType;
	strFailedRequestId = dispatcher.send(pendingCommand, 12);
	dispatcher.failAll(QStringLiteral("session_channel_closed"));
	Check(strFailureCode == QStringLiteral("session_channel_closed"),
		"closing the channel explicitly fails every pending command");

	KSessionCommandDispatcher failedSender;
	failedSender.setTransmitFunction([](const KSessionMessage &)
		{ return KSessionCommandTransmitResult { false, QStringLiteral("send_failed") }; });
	bool bSendFailureReported = false;
	QObject::connect(&failedSender, &KSessionCommandDispatcher::commandCompleted,
		[&](KSessionMessageType, const QString &strCompletedRequestId, bool bSuccess,
			const QString &strErrorCode, quint64 nGeneration)
		{
			bSendFailureReported = !strCompletedRequestId.isEmpty()
				&& !bSuccess
				&& strErrorCode == QStringLiteral("send_failed")
				&& nGeneration == 13;
		});
	KSessionMessage failedCommand;
	failedCommand.type = StartStreamingSessionMessageType;
	Check(failedSender.send(failedCommand, 13).isEmpty() && bSendFailureReported,
		"an initial transport failure is reported explicitly");

	return g_nFailureCount == 0 ? 0 : 1;
}
