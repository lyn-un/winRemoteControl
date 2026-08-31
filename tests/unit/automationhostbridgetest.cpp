#include "automation/automationhostbridge.h"
#include "commands/applicationcommandregistry.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionerror.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtCore/QThread>

#include <thread>

namespace
{
	class KFakeSessionController final : public KSessionController
	{
	public:
		void setRole(const QString &) override {}
		void startSignalingServer(quint16) override {}
		void connectSignaling(const QString &, quint16) override {}
		void retryLastConnection() override {}
		void disconnectSession() override {}
		void enterRemoteDesktop(const KStreamConfig &) override {}
		void leaveRemoteDesktop() override {}
		void startStreaming() override {}
		void stopStreaming() override {}
		void pushVideoFrame(const KVideoFrame &) override {}
		void sendInputMessage(const KInputMessage &) override {}
		void sendClipboardMessage(const KClipboardMessage &) override {}
		bool sendTerminalControlMessage(const KTerminalMessage &) override { return false; }
		bool sendTerminalData(const QByteArray &) override { return false; }
		bool isTerminalBackpressured() const override { return false; }
		void sendStreamConfig(const KStreamConfig &) override {}
		QString requestPrivacyMode(KPrivacyMode) override { return QString(); }
		QString requestPostSessionAction(KPostSessionAction) override { return QString(); }
		void handleCaptureFailure() override {}
		void applyApplicationSettings(const KApplicationSettings &) override {}
		void respondIncomingAccessRequest(const QString &, bool) override {}
		void respondPairingRequest(const QString &, bool, KPermissionScopes) override {}
		quint64 sessionGeneration() const override { return nGeneration; }
		KSessionRole sessionRole() const override { return ControllerSessionRole; }
		bool isIdle() const override { return bIdle; }
		bool matchesCurrentEndpoint(const QString &, quint16) const override { return false; }

		bool bIdle = true;
		quint64 nGeneration = 1;
	};

	struct KCallbackState
	{
		QEventLoop *pLoop = nullptr;
		QByteArray json;
		quint64 nRequestId = 0;
		QThread *pCallbackThread = nullptr;
		bool bExecutionStarted = false;
	};

	void CommandStarted(void *pContext, std::uint64_t)
	{
		auto *pState = static_cast<KCallbackState *>(pContext);
		pState->bExecutionStarted = true;
	}

	void JsonCompleted(void *pContext,
		std::uint64_t nRequestId,
		const char *pJson,
		std::uint32_t nJsonBytes)
	{
		auto *pState = static_cast<KCallbackState *>(pContext);
		pState->nRequestId = nRequestId;
		pState->json = QByteArray(pJson, static_cast<qsizetype>(nJsonBytes));
		pState->pCallbackThread = QThread::currentThread();
		pState->pLoop->quit();
	}

	bool WaitForCallback(KCallbackState *pState)
	{
		QTimer timer;
		timer.setSingleShot(true);
		QObject::connect(&timer, &QTimer::timeout, pState->pLoop, &QEventLoop::quit);
		timer.start(2000);
		pState->pLoop->exec();
		return timer.isActive() && !pState->json.isEmpty();
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	KApplicationCommandRegistry registry;
	KApplicationCommand command;
	command.strId = QStringLiteral("test.echo");
	command.execute = [](const QJsonObject &arguments)
	{
		KApplicationCommandResult result;
		result.status = ApplicationCommandSucceeded;
		result.value = arguments;
		return result;
	};
	if (!registry.registerCommand(command, nullptr))
		return 1;
	int nExpiredExecutionCount = 0;
	KApplicationCommand expiredCommand;
	expiredCommand.strId = QStringLiteral("test.expired");
	expiredCommand.execute = [&nExpiredExecutionCount](const QJsonObject &)
	{
		++nExpiredExecutionCount;
		KApplicationCommandResult result;
		result.status = ApplicationCommandSucceeded;
		return result;
	};
	if (!registry.registerCommand(expiredCommand, nullptr))
		return 2;
	int nOnceExecutionCount = 0;
	KApplicationCommand onceCommand;
	onceCommand.strId = QStringLiteral("test.once");
	onceCommand.execute = [&nOnceExecutionCount](const QJsonObject &)
	{
		++nOnceExecutionCount;
		KApplicationCommandResult result;
		result.status = ApplicationCommandSucceeded;
		return result;
	};
	if (!registry.registerCommand(onceCommand, nullptr))
		return 2;
	KFakeSessionController controller;
	KAutomationHostBridge bridge(&registry, &controller, QStringLiteral("test-data"));
	const KWrcDriverHostApiV2 *pApi = bridge.hostApi();

	QEventLoop commandLoop;
	KCallbackState commandState{&commandLoop};
	std::thread worker([pApi, &commandState]()
	{
		const QByteArray commandId = QByteArrayLiteral("test.echo");
		const QByteArray arguments = QByteArrayLiteral("{\"answer\":42}");
		pApi->submitCommand(pApi->pHostContext, 17,
			commandId.constData(), static_cast<std::uint32_t>(commandId.size()),
			arguments.constData(), static_cast<std::uint32_t>(arguments.size()),
			5000, &CommandStarted, &JsonCompleted, &commandState);
	});
	worker.join();
	if (!WaitForCallback(&commandState)
		|| commandState.nRequestId != 17
		|| !commandState.bExecutionStarted
		|| commandState.pCallbackThread != QThread::currentThread())
	{
		return 3;
	}
	const QJsonObject commandResponse = QJsonDocument::fromJson(commandState.json).object();
	if (commandResponse.value(QStringLiteral("status")).toInt(-1) != 0
		|| commandResponse.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("answer")).toInt() != 42)
	{
		return 4;
	}

	for (int nAttempt = 0; nAttempt < 2; ++nAttempt)
	{
		QEventLoop duplicateLoop;
		KCallbackState duplicateState{&duplicateLoop};
		const QByteArray duplicateCommandId = QByteArrayLiteral("test.once");
		const QByteArray duplicateArguments = QByteArrayLiteral("{}");
		pApi->submitCommand(pApi->pHostContext, 28,
			duplicateCommandId.constData(),
			static_cast<std::uint32_t>(duplicateCommandId.size()),
			duplicateArguments.constData(),
			static_cast<std::uint32_t>(duplicateArguments.size()),
			5000, &CommandStarted, &JsonCompleted, &duplicateState);
		if (!WaitForCallback(&duplicateState))
			return 4;
		const QJsonObject duplicateResponse = QJsonDocument::fromJson(
			duplicateState.json).object();
		if ((nAttempt == 0 && duplicateResponse.value(QStringLiteral("status")).toInt(-1) != 0)
			|| (nAttempt == 1
				&& duplicateResponse.value(QStringLiteral("errorCode")).toString()
					!= QStringLiteral("duplicate_request_id")))
		{
			return 4;
		}
	}
	if (nOnceExecutionCount != 1)
		return 4;

	QEventLoop expiredLoop;
	KCallbackState expiredState{&expiredLoop};
	std::thread expiredWorker([pApi, &expiredState]()
	{
		const QByteArray commandId = QByteArrayLiteral("test.expired");
		const QByteArray arguments = QByteArrayLiteral("{}");
		pApi->submitCommand(pApi->pHostContext, 24,
			commandId.constData(), static_cast<std::uint32_t>(commandId.size()),
			arguments.constData(), static_cast<std::uint32_t>(arguments.size()),
			5000, &CommandStarted, &JsonCompleted, &expiredState);
	});
	expiredWorker.join();
	QThread::msleep(5100);
	if (!WaitForCallback(&expiredState)
		|| expiredState.bExecutionStarted
		|| nExpiredExecutionCount != 0
		|| QJsonDocument::fromJson(expiredState.json).object()
			.value(QStringLiteral("errorCode")).toString()
			!= QStringLiteral("command_timeout"))
	{
		return 5;
	}

	emit controller.sessionStateChanged(StreamingSessionState);
	emit controller.privacyModeCommandCompleted(QStringLiteral("privacy-request"),
		false, QStringLiteral("permission_denied"));
	QEventLoop stateLoop;
	KCallbackState state{&stateLoop};
	const QByteArray kind = QByteArrayLiteral("state");
	pApi->requestSnapshot(pApi->pHostContext, 18,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &state);
	if (!WaitForCallback(&state))
		return 6;
	const QJsonObject snapshot = QJsonDocument::fromJson(state.json).object();
	if (snapshot.value(QStringLiteral("sessionState")).toString()
		!= QStringLiteral("Streaming")
		|| snapshot.value(QStringLiteral("webRtcState")).toString()
			!= QStringLiteral("connected")
		|| !snapshot.contains(QStringLiteral("receivedFrameCount"))
		|| snapshot.value(QStringLiteral("privacyModeStatus")).toObject()
			.value(QStringLiteral("errorCode")).toString()
			!= QStringLiteral("permission_denied")
		|| !snapshot.contains(QStringLiteral("postSessionActionStatus"))
		|| !snapshot.value(QStringLiteral("supportedCommands")).toArray()
			.contains(QStringLiteral("test.echo")))
	{
		return 7;
	}
	emit controller.incomingAccessRequest(QStringLiteral("access-before-frames"),
		QStringLiteral("device"), QString(), 12345);
	for (int i = 0; i < 600; ++i)
		emit controller.remoteFrameStatsReady(640, 480, static_cast<quint64>(i), i);
	QEventLoop eventsLoop;
	KCallbackState eventsState{&eventsLoop};
	const QByteArray eventsKind = QByteArrayLiteral("events");
	pApi->requestSnapshot(pApi->pHostContext, 20,
		eventsKind.constData(), static_cast<std::uint32_t>(eventsKind.size()), 0,
		&JsonCompleted, &eventsState);
	if (!WaitForCallback(&eventsState))
		return 8;
	const QJsonObject eventSnapshot = QJsonDocument::fromJson(eventsState.json).object();
	const QJsonArray events = eventSnapshot.value(QStringLiteral("events")).toArray();
	bool bFoundAccessRequest = false;
	int nFrameProgressEvents = 0;
	for (const QJsonValue &value : events)
	{
		const QJsonObject event = value.toObject();
		if (event.value(QStringLiteral("type")) == QStringLiteral("access.requested"))
		{
			bFoundAccessRequest = true;
			if (event.value(QStringLiteral("sessionGeneration")).toString()
				!= QStringLiteral("1"))
			{
				return 9;
			}
		}
		if (event.value(QStringLiteral("type")) == QStringLiteral("frame.progress"))
			++nFrameProgressEvents;
		if (event.value(QStringLiteral("sessionGeneration")).toString().isEmpty())
			return 9;
	}
	if (!bFoundAccessRequest || nFrameProgressEvents > 1)
	{
		return 9;
	}
	const quint64 nGapBaseline = eventSnapshot.value(QStringLiteral("nextSequence"))
		.toString().toULongLong() - 1;
	for (int i = 0; i < 250; ++i)
		emit controller.sessionStateChanged((i % 2) == 0
			? ConnectedSessionState : StreamingSessionState);
	QEventLoop gapLoop;
	KCallbackState gapState{&gapLoop};
	pApi->requestSnapshot(pApi->pHostContext, 25,
		eventsKind.constData(), static_cast<std::uint32_t>(eventsKind.size()),
		nGapBaseline, &JsonCompleted, &gapState);
	if (!WaitForCallback(&gapState)
		|| !QJsonDocument::fromJson(gapState.json).object()
			.value(QStringLiteral("hasGap")).toBool())
	{
		return 9;
	}

	KSessionError previousError;
	previousError.domain = SignalingSessionErrorDomain;
	previousError.code = ConnectionLostSessionErrorCode;
	previousError.stage = ConnectedSessionErrorStage;
	previousError.bRetryable = true;
	previousError.strTechnicalMessage = QStringLiteral("previous generation");
	emit controller.sessionErrorOccurred(previousError);
	emit controller.sessionStateChanged(ReconnectingSessionState);
	QEventLoop failureLoop;
	KCallbackState failureState{&failureLoop};
	pApi->requestSnapshot(pApi->pHostContext, 26,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &failureState);
	const QJsonObject failureSnapshot = WaitForCallback(&failureState)
		? QJsonDocument::fromJson(failureState.json).object() : QJsonObject();
	const QJsonObject currentError = failureSnapshot.value(
		QStringLiteral("currentError")).toObject();
	if (currentError.value(QStringLiteral("code")).toString()
			!= QStringLiteral("connection_lost")
		|| currentError.value(QStringLiteral("domain")).toString()
			!= QStringLiteral("signaling")
		|| currentError.value(QStringLiteral("stage")).toString()
			!= QStringLiteral("connected")
		|| !currentError.value(QStringLiteral("retryable")).toBool()
		|| currentError.value(QStringLiteral("technicalMessage")).toString()
			!= QStringLiteral("previous generation")
		|| currentError.value(QStringLiteral("occurredAtMs")).toString().isEmpty()
		|| currentError.value(QStringLiteral("sessionGeneration")).toString()
			!= QStringLiteral("1"))
	{
		return 10;
	}
	emit controller.sessionStateChanged(ConnectedSessionState);
	QEventLoop recoveryLoop;
	KCallbackState recoveryState{&recoveryLoop};
	pApi->requestSnapshot(pApi->pHostContext, 27,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &recoveryState);
	const QJsonObject recoverySnapshot = WaitForCallback(&recoveryState)
		? QJsonDocument::fromJson(recoveryState.json).object() : QJsonObject();
	if (!recoverySnapshot.value(QStringLiteral("currentError")).isNull()
		|| recoverySnapshot.value(QStringLiteral("lastError")).toObject()
			.value(QStringLiteral("code")).toString() != QStringLiteral("connection_lost"))
	{
		return 10;
	}
	emit controller.remoteFrameStatsReady(1920, 1080, 1, 1234);

	QEventLoop staleLoop;
	KCallbackState staleState{&staleLoop};
	std::thread staleWorker([pApi, &staleState]()
	{
		const QByteArray staleCommandId = QByteArrayLiteral("test.echo");
		const QByteArray staleArguments = QByteArrayLiteral("{}");
		pApi->submitCommand(pApi->pHostContext, 21,
			staleCommandId.constData(),
			static_cast<std::uint32_t>(staleCommandId.size()),
			staleArguments.constData(),
			static_cast<std::uint32_t>(staleArguments.size()),
			5000, &CommandStarted, &JsonCompleted, &staleState);
	});
	staleWorker.join();
	controller.nGeneration = 2;
	if (!WaitForCallback(&staleState))
		return 10;
	const QJsonObject staleResponse = QJsonDocument::fromJson(staleState.json).object();
	if (staleResponse.value(QStringLiteral("errorCode")).toString()
		!= QStringLiteral("stale_generation"))
	{
		return 11;
	}
	emit controller.webRtcStateChanged(QStringLiteral("connected"));
	emit controller.sessionStateChanged(ConnectingSessionState);
	QEventLoop resetLoop;
	KCallbackState resetState{&resetLoop};
	pApi->requestSnapshot(pApi->pHostContext, 22,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &resetState);
	if (!WaitForCallback(&resetState))
		return 12;
	const QJsonObject resetSnapshot = QJsonDocument::fromJson(resetState.json).object();
	if (!resetSnapshot.value(QStringLiteral("currentError")).isNull()
		|| resetSnapshot.value(QStringLiteral("lastError")).toObject()
			.value(QStringLiteral("code")).toString() != QStringLiteral("connection_lost")
		|| resetSnapshot.value(QStringLiteral("lastError")).toObject()
			.value(QStringLiteral("sessionGeneration")).toString() != QStringLiteral("1")
		|| resetSnapshot.value(QStringLiteral("receivedFrameCount")).toString()
			!= QStringLiteral("0")
		|| resetSnapshot.value(QStringLiteral("webRtcState")).toString()
			!= QStringLiteral("connected")
		|| !resetSnapshot.value(QStringLiteral("privacyModeStatus")).toObject()
			.value(QStringLiteral("errorCode")).toString().isEmpty())
	{
		return 13;
	}
	KSessionError currentGenerationError;
	currentGenerationError.domain = SignalingSessionErrorDomain;
	currentGenerationError.code = ConnectionLostSessionErrorCode;
	currentGenerationError.stage = ConnectedSessionErrorStage;
	currentGenerationError.bRetryable = true;
	currentGenerationError.strTechnicalMessage = QStringLiteral("current generation");
	emit controller.sessionErrorOccurred(currentGenerationError);
	QEventLoop generationErrorLoop;
	KCallbackState generationErrorState{&generationErrorLoop};
	pApi->requestSnapshot(pApi->pHostContext, 29,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &generationErrorState);
	const QJsonObject generationErrorSnapshot = WaitForCallback(&generationErrorState)
		? QJsonDocument::fromJson(generationErrorState.json).object() : QJsonObject();
	if (generationErrorSnapshot.value(QStringLiteral("currentError")).toObject()
			.value(QStringLiteral("sessionGeneration")).toString() != QStringLiteral("2")
		|| generationErrorSnapshot.value(QStringLiteral("lastError")).toObject()
			.value(QStringLiteral("technicalMessage")).toString()
			!= QStringLiteral("current generation"))
	{
		return 13;
	}
	QEventLoop generationEventsLoop;
	KCallbackState generationEventsState{&generationEventsLoop};
	pApi->requestSnapshot(pApi->pHostContext, 30,
		eventsKind.constData(), static_cast<std::uint32_t>(eventsKind.size()), 0,
		&JsonCompleted, &generationEventsState);
	const QJsonArray generationEvents = WaitForCallback(&generationEventsState)
		? QJsonDocument::fromJson(generationEventsState.json).object()
			.value(QStringLiteral("events")).toArray() : QJsonArray();
	bool bFoundGenerationOneError = false;
	bool bFoundGenerationTwoError = false;
	for (const QJsonValue &value : generationEvents)
	{
		const QJsonObject event = value.toObject();
		if (event.value(QStringLiteral("type")).toString()
			!= QStringLiteral("session.error"))
		{
			continue;
		}
		bFoundGenerationOneError = bFoundGenerationOneError
			|| event.value(QStringLiteral("sessionGeneration")).toString()
				== QStringLiteral("1");
		bFoundGenerationTwoError = bFoundGenerationTwoError
			|| event.value(QStringLiteral("sessionGeneration")).toString()
				== QStringLiteral("2");
	}
	if (!bFoundGenerationOneError || !bFoundGenerationTwoError)
		return 13;
	emit controller.sessionStateChanged(ConnectedSessionState);
	const qint64 nBeforeFrameReceivedMs = QDateTime::currentMSecsSinceEpoch();
	emit controller.remoteFrameStatsReady(1280, 720, 2, 0);
	QEventLoop frameLoop;
	KCallbackState frameState{&frameLoop};
	pApi->requestSnapshot(pApi->pHostContext, 23,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &frameState);
	if (!WaitForCallback(&frameState))
		return 14;
	const QJsonObject frameSnapshot = QJsonDocument::fromJson(frameState.json).object();
	const qint64 nFrameReceivedAtMs = frameSnapshot
		.value(QStringLiteral("lastFrameTimestampMs")).toString().toLongLong();
	if (frameSnapshot.value(QStringLiteral("receivedFrameCount")).toString()
			!= QStringLiteral("1")
		|| frameSnapshot.value(QStringLiteral("lastFrameWidth")).toInt() != 1280
		|| frameSnapshot.value(QStringLiteral("lastFrameHeight")).toInt() != 720
		|| nFrameReceivedAtMs < nBeforeFrameReceivedMs
		|| nFrameReceivedAtMs > QDateTime::currentMSecsSinceEpoch())
	{
		return 15;
	}

	bridge.stopAcceptingRequests();
	QEventLoop shutdownLoop;
	KCallbackState shutdownState{&shutdownLoop};
	const QByteArray commandId = QByteArrayLiteral("test.echo");
	const QByteArray arguments = QByteArrayLiteral("{}");
	pApi->submitCommand(pApi->pHostContext, 19,
		commandId.constData(), static_cast<std::uint32_t>(commandId.size()),
		arguments.constData(), static_cast<std::uint32_t>(arguments.size()),
		5000, &CommandStarted, &JsonCompleted, &shutdownState);
	if (!WaitForCallback(&shutdownState))
		return 16;
	const QJsonObject shutdownResponse = QJsonDocument::fromJson(shutdownState.json).object();
	if (shutdownResponse.value(QStringLiteral("errorCode")).toString()
		!= QStringLiteral("application_shutdown"))
	{
		return 17;
	}
	return 0;
}
