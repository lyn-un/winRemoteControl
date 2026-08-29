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
	};

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
	KFakeSessionController controller;
	KAutomationHostBridge bridge(&registry, &controller, QStringLiteral("test-data"));
	const KWrcDriverHostApiV1 *pApi = bridge.hostApi();

	QEventLoop commandLoop;
	KCallbackState commandState{&commandLoop};
	std::thread worker([pApi, &commandState]()
	{
		const QByteArray commandId = QByteArrayLiteral("test.echo");
		const QByteArray arguments = QByteArrayLiteral("{\"answer\":42}");
		pApi->submitCommand(pApi->pHostContext, 17,
			commandId.constData(), static_cast<std::uint32_t>(commandId.size()),
			arguments.constData(), static_cast<std::uint32_t>(arguments.size()),
			&JsonCompleted, &commandState);
	});
	worker.join();
	if (!WaitForCallback(&commandState)
		|| commandState.nRequestId != 17
		|| commandState.pCallbackThread != QThread::currentThread())
	{
		return 2;
	}
	const QJsonObject commandResponse = QJsonDocument::fromJson(commandState.json).object();
	if (commandResponse.value(QStringLiteral("status")).toInt(-1) != 0
		|| commandResponse.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("answer")).toInt() != 42)
	{
		return 3;
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
		return 4;
	const QJsonObject snapshot = QJsonDocument::fromJson(state.json).object();
	if (snapshot.value(QStringLiteral("sessionState")).toString()
		!= QStringLiteral("Streaming")
		|| snapshot.value(QStringLiteral("webRtcState")).toString()
			!= QStringLiteral("connected")
		|| !snapshot.contains(QStringLiteral("receivedFrameCount"))
		|| snapshot.value(QStringLiteral("privacyModeStatus")).toObject()
			.value(QStringLiteral("errorCode")).toString()
			!= QStringLiteral("permission_denied")
		|| !snapshot.contains(QStringLiteral("postSessionActionStatus")))
	{
		return 5;
	}
	for (int i = 0; i < 600; ++i)
		emit controller.sessionStateChanged((i % 2) == 0
			? ConnectedSessionState : StreamingSessionState);
	QEventLoop eventsLoop;
	KCallbackState eventsState{&eventsLoop};
	const QByteArray eventsKind = QByteArrayLiteral("events");
	pApi->requestSnapshot(pApi->pHostContext, 20,
		eventsKind.constData(), static_cast<std::uint32_t>(eventsKind.size()), 0,
		&JsonCompleted, &eventsState);
	if (!WaitForCallback(&eventsState))
		return 6;
	const QJsonArray events = QJsonDocument::fromJson(eventsState.json).object()
		.value(QStringLiteral("events")).toArray();
	if (events.size() != 512
		|| events.first().toObject().value(QStringLiteral("sequence")).toString().toULongLong()
		>= events.last().toObject().value(QStringLiteral("sequence")).toString().toULongLong())
	{
		return 7;
	}

	KSessionError previousError;
	previousError.code = ConnectionLostSessionErrorCode;
	previousError.strTechnicalMessage = QStringLiteral("previous generation");
	emit controller.sessionErrorOccurred(previousError);
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
			&JsonCompleted, &staleState);
	});
	staleWorker.join();
	controller.nGeneration = 2;
	if (!WaitForCallback(&staleState))
		return 8;
	const QJsonObject staleResponse = QJsonDocument::fromJson(staleState.json).object();
	if (staleResponse.value(QStringLiteral("errorCode")).toString()
		!= QStringLiteral("stale_generation"))
	{
		return 9;
	}
	emit controller.webRtcStateChanged(QStringLiteral("connected"));
	emit controller.sessionStateChanged(ConnectingSessionState);
	QEventLoop resetLoop;
	KCallbackState resetState{&resetLoop};
	pApi->requestSnapshot(pApi->pHostContext, 22,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &resetState);
	if (!WaitForCallback(&resetState))
		return 10;
	const QJsonObject resetSnapshot = QJsonDocument::fromJson(resetState.json).object();
	if (!resetSnapshot.value(QStringLiteral("lastError")).toString().isEmpty()
		|| resetSnapshot.value(QStringLiteral("receivedFrameCount")).toString()
			!= QStringLiteral("0")
		|| resetSnapshot.value(QStringLiteral("webRtcState")).toString()
			!= QStringLiteral("connected")
		|| !resetSnapshot.value(QStringLiteral("privacyModeStatus")).toObject()
			.value(QStringLiteral("errorCode")).toString().isEmpty())
	{
		return 11;
	}
	const qint64 nBeforeFrameReceivedMs = QDateTime::currentMSecsSinceEpoch();
	emit controller.remoteFrameStatsReady(1280, 720, 2, 0);
	QEventLoop frameLoop;
	KCallbackState frameState{&frameLoop};
	pApi->requestSnapshot(pApi->pHostContext, 23,
		kind.constData(), static_cast<std::uint32_t>(kind.size()), 0,
		&JsonCompleted, &frameState);
	if (!WaitForCallback(&frameState))
		return 12;
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
		return 13;
	}

	bridge.stopAcceptingRequests();
	QEventLoop shutdownLoop;
	KCallbackState shutdownState{&shutdownLoop};
	const QByteArray commandId = QByteArrayLiteral("test.echo");
	const QByteArray arguments = QByteArrayLiteral("{}");
	pApi->submitCommand(pApi->pHostContext, 19,
		commandId.constData(), static_cast<std::uint32_t>(commandId.size()),
		arguments.constData(), static_cast<std::uint32_t>(arguments.size()),
		&JsonCompleted, &shutdownState);
	if (!WaitForCallback(&shutdownState))
		return 14;
	const QJsonObject shutdownResponse = QJsonDocument::fromJson(shutdownState.json).object();
	if (shutdownResponse.value(QStringLiteral("errorCode")).toString()
		!= QStringLiteral("application_shutdown"))
	{
		return 15;
	}
	return 0;
}
