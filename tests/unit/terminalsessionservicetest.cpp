#include "terminal/terminalsessionservice.h"

#include "core/terminal/terminalhost.h"
#include "core/terminal/terminalfrontend.h"
#include "core/protocol/terminaldataframe.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QUuid>

#include <algorithm>
#include <iostream>
#include <memory>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	class KFakeTerminalHost final : public KTerminalHost
	{
	public:
		bool isSupported(QString *) const override { return true; }
		bool start(quint64 nGeneration, int nColumns, int nRows,
			QString *pErrorMessage) override
		{
			if (!bStartSucceeds)
			{
				if (pErrorMessage != nullptr)
					*pErrorMessage = QStringLiteral("injected start failure");
				return false;
			}
			++nStartCount;
			nLastGeneration = nGeneration;
			nLastColumns = nColumns;
			nLastRows = nRows;
			return true;
		}
		bool writeInput(quint64, const QByteArray &data) override
		{
			if (!bWriteSucceeds)
				return false;
			input.append(data);
			return true;
		}
		bool resize(quint64, int nColumns, int nRows) override
		{
			nLastColumns = nColumns;
			nLastRows = nRows;
			return true;
		}
		void requestStop(quint64 nGeneration) override
		{
			++nStopCount;
			if (bEmitStopped)
				emit stopped(nGeneration);
		}
		void fail(quint64 nGeneration)
		{
			emit terminalError(nGeneration, QStringLiteral("runtime_failed"),
				QStringLiteral("injected runtime failure"));
		}
		void finishStop(quint64 nGeneration)
		{
			emit stopped(nGeneration);
		}

		int nStartCount = 0;
		int nStopCount = 0;
		int nLastColumns = 0;
		int nLastRows = 0;
		quint64 nLastGeneration = 0;
		QByteArray input;
		bool bStartSucceeds = true;
		bool bWriteSucceeds = true;
		bool bEmitStopped = true;
	};

	class KFakeTerminalFrontend final : public KTerminalFrontend
	{
	public:
		bool isSupported(QString *) const override { return true; }
		bool open(quint64 nGeneration, const QString &, QString *) override
		{
			nCurrentGeneration = nGeneration;
			emit connected(nGeneration);
			return true;
		}
		void focus() override { ++nFocusCount; }
		bool writeOutput(quint64, const QByteArray &data) override
		{
			if (!bWriteSucceeds)
				return false;
			output.append(data);
			return true;
		}
		void setInputPaused(bool bPaused) override
		{
			bInputPaused = bPaused;
			++nInputPauseChanges;
		}
		void close(quint64) override { ++nCloseCount; }
		void failHandshake(quint64 nGeneration)
		{
			emit terminalError(nGeneration, QStringLiteral("relay_auth_failed"),
				QStringLiteral("injected handshake failure"));
		}
		void closeFromUser(quint64 nGeneration)
		{
			emit closed(nGeneration);
		}

		quint64 nCurrentGeneration = 0;
		int nFocusCount = 0;
		int nCloseCount = 0;
		int nInputPauseChanges = 0;
		QByteArray output;
		bool bWriteSucceeds = true;
		bool bInputPaused = false;
	};

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
		bool sendTerminalControlMessage(const KTerminalMessage &message) override
		{
			if (bSendControlFails)
				return false;
			messages.append(message);
			return true;
		}
		bool sendTerminalData(const QByteArray &data) override
		{
			if (bSendDataFails)
				return false;
			output.append(data);
			dataMessages.append(data);
			return true;
		}
		bool isTerminalBackpressured() const override { return bBackpressured; }
		void sendStreamConfig(const KStreamConfig &) override {}
		QString requestPrivacyMode(KPrivacyMode) override { return QString(); }
		QString requestPostSessionAction(KPostSessionAction) override { return QString(); }
		void handleCaptureFailure() override {}
		void applyApplicationSettings(const KApplicationSettings &) override {}
		void respondIncomingAccessRequest(const QString &, bool) override {}
		void respondPairingRequest(const QString &, bool,
			KPermissionScopes) override {}
		quint64 sessionGeneration() const override { return nGeneration; }
		KSessionRole sessionRole() const override { return ControllerSessionRole; }
		bool isIdle() const override { return false; }
		bool matchesCurrentEndpoint(const QString &, quint16) const override { return true; }

		void makeReady()
		{
			emitConnected();
			emitCapabilities();
			emitChannel(true);
		}

		void emitConnected()
		{
			emit sessionStateChanged(ConnectedSessionState);
		}

		void emitCapabilities()
		{
			KNegotiatedCapabilities capabilities;
			capabilities.bValid = true;
			capabilities.channels.append(QStringLiteral("terminal"));
			emit sessionCapabilitiesChanged(capabilities);
		}

		void emitChannel(bool bOpen)
		{
			emit terminalChannelChanged(bOpen);
		}

		quint64 nGeneration = 7;
		QVector<KTerminalMessage> messages;
		QByteArray output;
		QVector<QByteArray> dataMessages;
		bool bBackpressured = false;
		bool bSendDataFails = false;
		bool bSendControlFails = false;
	};

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

	QByteArray EncodeTerminalData(const QString &strRequestId,
		KTerminalDataDirection direction,
		quint64 nSequence,
		const QByteArray &payload)
	{
		KTerminalDataFrame frame;
		frame.strRequestId = strRequestId;
		frame.direction = direction;
		frame.nSequence = nSequence;
		frame.payload = payload;
		return KTerminalDataFrameCodec::encode(frame);
	}

	void AssignCommandId(KTerminalMessage *pMessage)
	{
		pMessage->strCommandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}

	void AcknowledgeLastCommand(KFakeSessionController *pController)
	{
		const KTerminalMessage command = pController->messages.last();
		KTerminalMessage result;
		result.type = CommandResultTerminalMessageType;
		result.strRequestId = command.strRequestId;
		result.strCommandId = command.strCommandId;
		result.bSuccess = true;
		emit pController->terminalControlMessageReceived(result);
	}

	void TestControllerRequestsApproval()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		controller.makeReady();
		service.openCurrentTerminal(120, 40);
		Check(controller.messages.size() == 1,
			QStringLiteral("controller sends one terminal request"));
		Check(controller.messages.first().type == OpenRequestTerminalMessageType,
			QStringLiteral("controller request has open type"));
		Check(controller.messages.first().nColumns == 120
			&& controller.messages.first().nRows == 40,
			QStringLiteral("controller request preserves terminal size"));
	}

	void TestControlledApprovalStartsSingleHost()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
		controller.makeReady();

		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QStringLiteral("17698aa1-9108-405c-a0eb-dc1b78777ad4");
		request.nColumns = 90;
		request.nRows = 28;
		AssignCommandId(&request);
		emit controller.terminalControlMessageReceived(request);
		Check(std::any_of(controller.messages.cbegin(), controller.messages.cend(),
			[](const KTerminalMessage &message)
			{
				return message.type == ApprovalPendingTerminalMessageType;
			}),
			QStringLiteral("controlled side reports approval pending"));

		service.respondIncomingRequest(request.strRequestId, true);
		Check(pHost->nStartCount == 1,
			QStringLiteral("approval starts exactly one ConPTY host"));
		Check(controller.messages.last().type == AcceptedTerminalMessageType,
			QStringLiteral("host start is acknowledged after success"));
		AcknowledgeLastCommand(&controller);

		emit controller.terminalDataReceived(EncodeTerminalData(request.strRequestId,
			InputTerminalDataDirection, 1, QByteArray("dir\r\n")));
		Check(pHost->input == QByteArray("dir\r\n"),
			QStringLiteral("terminal input reaches controlled host"));
	}

	void TestPendingOpenHandlesEveryReadyEventOrder()
	{
		for (int nOrder = 0; nOrder < 6; ++nOrder)
		{
			auto spHost = std::make_unique<KFakeTerminalHost>();
			auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
			KFakeSessionController controller;
			KTerminalSessionService service(std::move(spHost), &controller,
				std::move(spFrontend));
			service.openCurrentTerminal(100, 30);
			const int events[6][3] = {
				{ 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
				{ 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 }
			};
			for (int nIndex = 0; nIndex < 3; ++nIndex)
			{
				switch (events[nOrder][nIndex])
				{
				case 0: controller.emitConnected(); break;
				case 1: controller.emitCapabilities(); break;
				case 2: controller.emitChannel(true); break;
				}
			}
			controller.emitConnected();
			controller.emitCapabilities();
			controller.emitChannel(true);
			Check(controller.messages.size() == 1,
				QStringLiteral("pending terminal request is sent once for event order %1")
					.arg(nOrder));
			Check(!controller.messages.isEmpty()
				&& controller.messages.first().type == OpenRequestTerminalMessageType,
				QStringLiteral("pending terminal request has open type for order %1")
					.arg(nOrder));
		}
	}

	void TestOutputBeforeAcceptedIsPreserved()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		controller.makeReady();
		QByteArray output;
		QObject::connect(&service, &KTerminalSessionService::outputReady,
			[&output](const QByteArray &data) { output.append(data); });
		service.openCurrentTerminal();
		emit controller.terminalDataReceived(EncodeTerminalData(
			controller.messages.first().strRequestId,
			OutputTerminalDataDirection, 1, QByteArray("PowerShell banner\r\n")));
		Check(output.isEmpty(),
			QStringLiteral("early terminal output waits for acceptance"));

		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		Check(output == QByteArray("PowerShell banner\r\n"),
			QStringLiteral("early terminal output is flushed after acceptance"));
	}

	void TestOldAndOutOfOrderDataIsRejected()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
		controller.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QStringLiteral("17698aa1-9108-405c-a0eb-dc1b78777ad4");
		request.nColumns = 90;
		request.nRows = 28;
		AssignCommandId(&request);
		emit controller.terminalControlMessageReceived(request);
		service.respondIncomingRequest(request.strRequestId, true);
		AcknowledgeLastCommand(&controller);

		emit controller.terminalDataReceived(EncodeTerminalData(
			QStringLiteral("550e8400-e29b-41d4-a716-446655440000"),
			InputTerminalDataDirection, 1, QByteArray("old")));
		emit controller.terminalDataReceived(EncodeTerminalData(request.strRequestId,
			InputTerminalDataDirection, 2, QByteArray("new")));
		emit controller.terminalDataReceived(EncodeTerminalData(request.strRequestId,
			InputTerminalDataDirection, 2, QByteArray("duplicate")));
		emit controller.terminalDataReceived(EncodeTerminalData(request.strRequestId,
			InputTerminalDataDirection, 1, QByteArray("late")));
		Check(pHost->input == QByteArray("new"),
			QStringLiteral("old instance and non-increasing terminal data are rejected"));
	}

	void TestInputBackpressureQueuesAndFlushesInOrder()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeTerminalFrontend *pFrontend = spFrontend.get();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		controller.makeReady();
		service.openCurrentTerminal();
		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		controller.bBackpressured = true;
		service.sendInput(QByteArray("first"));
		service.sendInput(QByteArray("second"));
		Check(controller.dataMessages.isEmpty(),
			QStringLiteral("backpressured terminal input is queued"));
		Check(pFrontend->bInputPaused,
			QStringLiteral("relay input reading pauses while data channel is congested"));
		controller.bBackpressured = false;
		emit controller.terminalLowWatermarkReached();
		Check(controller.dataMessages.size() == 2,
			QStringLiteral("queued terminal input flushes after low watermark"));
		Check(!pFrontend->bInputPaused,
			QStringLiteral("relay input reading resumes at low watermark"));
		KTerminalDataFrame first;
		KTerminalDataFrame second;
		Check(KTerminalDataFrameCodec::decode(controller.dataMessages.value(0), &first)
			&& KTerminalDataFrameCodec::decode(controller.dataMessages.value(1), &second)
			&& first.payload == QByteArray("first")
			&& second.payload == QByteArray("second")
			&& first.nSequence == 1 && second.nSequence == 2,
			QStringLiteral("queued terminal input preserves bytes and sequence"));
	}

	void TestRejectAndDuplicateApprovalDoNotStartHost()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
		controller.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controller.terminalControlMessageReceived(request);
		service.respondIncomingRequest(request.strRequestId, false);
		service.respondIncomingRequest(request.strRequestId, true);
		Check(pHost->nStartCount == 0,
			QStringLiteral("rejected and duplicate approvals do not start host"));
	}

	void TestChannelCloseAndReconnectLifecycle()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		controller.makeReady();
		QVector<KTerminalState> states;
		QObject::connect(&service, &KTerminalSessionService::stateChanged,
			[&states](KTerminalState state, bool, const QString &,
				const QString &, const QString &) { states.append(state); });
		service.openCurrentTerminal();
		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		emit controller.sessionStateChanged(ReconnectingSessionState);
		emit controller.sessionStateChanged(ConnectedSessionState);
		emit controller.terminalChannelChanged(false);
		Check(states.contains(PausedTerminalState)
			&& states.contains(RunningTerminalState)
			&& states.last() == ClosedTerminalState,
			QStringLiteral("reconnect pauses and channel close converges to closed"));
	}

	void TestInputOverflowAndHostFailuresAreStructured()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		pHost->bWriteSucceeds = false;
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
		QVector<KSessionError> errors;
		QObject::connect(&service, &KTerminalSessionService::structuredTerminalError,
			[&errors](const KSessionError &error) { errors.append(error); });
		controller.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controller.terminalControlMessageReceived(request);
		service.respondIncomingRequest(request.strRequestId, true);
		AcknowledgeLastCommand(&controller);
		emit controller.terminalDataReceived(EncodeTerminalData(request.strRequestId,
			InputTerminalDataDirection, 1, QByteArray("input")));
		Check(!errors.isEmpty()
			&& errors.last().code == TerminalInputOverflowSessionErrorCode,
			QStringLiteral("host input failure emits structured overflow error"));
	}

	void TestControlSendFailureIsStructured()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		controller.bSendControlFails = true;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		QVector<KSessionError> errors;
		QObject::connect(&service, &KTerminalSessionService::structuredTerminalError,
			[&errors](const KSessionError &error) { errors.append(error); });
		controller.makeReady();
		service.openCurrentTerminal();
		Check(!errors.isEmpty()
			&& errors.last().domain == TerminalSessionErrorDomain,
			QStringLiteral("control send failure uses structured terminal error"));
	}

	void TestHostStartFailureAndFrontendWriteFailureAreReported()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		pHost->bStartSucceeds = false;
		KFakeSessionController controlled;
		KTerminalSessionService controlledService(std::move(spHost), &controlled);
		QVector<KSessionError> controlledErrors;
		QObject::connect(&controlledService,
			&KTerminalSessionService::structuredTerminalError,
			[&controlledErrors](const KSessionError &error)
				{ controlledErrors.append(error); });
		controlled.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controlled.terminalControlMessageReceived(request);
		controlledService.respondIncomingRequest(request.strRequestId, true);
		Check(!controlledErrors.isEmpty()
			&& controlledErrors.last().code == TerminalHostStartFailedSessionErrorCode,
			QStringLiteral("host startup failure is structured"));

		auto spFrontendHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeTerminalFrontend *pFrontend = spFrontend.get();
		pFrontend->bWriteSucceeds = false;
		KFakeSessionController controller;
		KTerminalSessionService controllerService(std::move(spFrontendHost),
			&controller, std::move(spFrontend));
		QVector<KSessionError> controllerErrors;
		QObject::connect(&controllerService,
			&KTerminalSessionService::structuredTerminalError,
			[&controllerErrors](const KSessionError &error)
				{ controllerErrors.append(error); });
		controller.makeReady();
		controllerService.openCurrentTerminal();
		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		emit controller.terminalDataReceived(EncodeTerminalData(
			accepted.strRequestId, OutputTerminalDataDirection, 1,
			QByteArray("output")));
		Check(!controllerErrors.isEmpty()
			&& controllerErrors.last().domain == TerminalSessionErrorDomain,
			QStringLiteral("frontend write failure is structured"));
	}

	void TestSessionEndClearsQueuedInput()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		controller.makeReady();
		service.openCurrentTerminal();
		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		controller.bBackpressured = true;
		service.sendInput(QByteArray("queued-old-input"));
		emit controller.sessionStateChanged(IdleSessionState);
		controller.bBackpressured = false;
		emit controller.terminalLowWatermarkReached();
		Check(controller.dataMessages.isEmpty(),
			QStringLiteral("session end discards queued terminal input"));
	}

	void TestEveryOpenRequiresApproval()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
		controller.makeReady();
		KTerminalMessage first;
		first.type = OpenRequestTerminalMessageType;
		first.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		first.nColumns = 100;
		first.nRows = 30;
		AssignCommandId(&first);
		emit controller.terminalControlMessageReceived(first);
		service.respondIncomingRequest(first.strRequestId, true);
		AcknowledgeLastCommand(&controller);
		KTerminalMessage close;
		close.type = CloseTerminalMessageType;
		close.strRequestId = first.strRequestId;
		close.strReason = QStringLiteral("test_close");
		AssignCommandId(&close);
		emit controller.terminalControlMessageReceived(close);
		KTerminalMessage second = first;
		second.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		AssignCommandId(&second);
		emit controller.terminalControlMessageReceived(second);
		Check(pHost->nStartCount == 1
			&& controller.messages.last().type == CommandResultTerminalMessageType,
			QStringLiteral("second open waits for a new approval"));
		service.respondIncomingRequest(second.strRequestId, true);
		Check(pHost->nStartCount == 2,
			QStringLiteral("second explicit approval starts the new host"));
	}

	void TestAllTerminalQueuesFailClosedOnOverflow()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		QVector<KSessionError> errors;
		QObject::connect(&service, &KTerminalSessionService::structuredTerminalError,
			[&errors](const KSessionError &error) { errors.append(error); });
		controller.makeReady();
		service.openCurrentTerminal();
		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		controller.bBackpressured = true;
		for (int nIndex = 0; nIndex < 20; ++nIndex)
			service.sendInput(QByteArray(16 * 1024, 'i'));
		Check(!errors.isEmpty()
			&& errors.last().code == TerminalInputOverflowSessionErrorCode,
			QStringLiteral("controller input queue overflow fails closed"));

		auto spOutputHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pOutputHost = spOutputHost.get();
		KFakeSessionController controlled;
		controlled.bBackpressured = true;
		KTerminalSessionService outputService(std::move(spOutputHost), &controlled);
		QVector<KSessionError> outputErrors;
		QObject::connect(&outputService,
			&KTerminalSessionService::structuredTerminalError,
			[&outputErrors](const KSessionError &error) { outputErrors.append(error); });
		controlled.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controlled.terminalControlMessageReceived(request);
		outputService.respondIncomingRequest(request.strRequestId, true);
		AcknowledgeLastCommand(&controlled);
		for (int nIndex = 0; nIndex < 65; ++nIndex)
			emit pOutputHost->outputReady(controlled.nGeneration,
				QByteArray(16 * 1024, 'o'));
		Check(!outputErrors.isEmpty()
			&& outputErrors.last().code == TerminalOutputOverflowSessionErrorCode,
			QStringLiteral("controlled output queue overflow fails closed"));
	}

	void TestHostRuntimeAndFrontendLifecycleFailures()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		KFakeSessionController controlled;
		KTerminalSessionService hostService(std::move(spHost), &controlled);
		QVector<KSessionError> hostErrors;
		QObject::connect(&hostService, &KTerminalSessionService::structuredTerminalError,
			[&hostErrors](const KSessionError &error) { hostErrors.append(error); });
		controlled.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controlled.terminalControlMessageReceived(request);
		hostService.respondIncomingRequest(request.strRequestId, true);
		AcknowledgeLastCommand(&controlled);
		pHost->fail(controlled.nGeneration);
		Check(!hostErrors.isEmpty() && pHost->nStopCount > 0
			&& hostErrors.last().strTechnicalMessage
				== QStringLiteral("injected runtime failure"),
			QStringLiteral("host runtime failure reports and requests shutdown"));

		auto spFrontendHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeTerminalFrontend *pFrontend = spFrontend.get();
		KFakeSessionController controller;
		KTerminalSessionService frontendService(std::move(spFrontendHost),
			&controller, std::move(spFrontend));
		QVector<KSessionError> frontendErrors;
		QObject::connect(&frontendService,
			&KTerminalSessionService::structuredTerminalError,
			[&frontendErrors](const KSessionError &error)
				{ frontendErrors.append(error); });
		controller.makeReady();
		frontendService.openCurrentTerminal();
		pFrontend->failHandshake(controller.nGeneration);
		Check(!frontendErrors.isEmpty()
			&& frontendErrors.last().code
				== TerminalRelayHandshakeFailedSessionErrorCode,
			QStringLiteral("frontend handshake failure is structured"));
		pFrontend->closeFromUser(controller.nGeneration);
		Check(pFrontend->nCloseCount > 0,
			QStringLiteral("frontend user close converges through terminal shutdown"));
	}

	void TestApprovalTimeoutAndRecoveryFailureConverge()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		KFakeSessionController controlled;
		KTerminalSessionService approvalService(std::move(spHost), &controlled);
		approvalService.setApprovalTimeoutSeconds(10);
		controlled.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controlled.terminalControlMessageReceived(request);
		const auto pendingIterator = std::find_if(controlled.messages.cbegin(),
			controlled.messages.cend(), [](const KTerminalMessage &message)
				{ return message.type == ApprovalPendingTerminalMessageType; });
		if (pendingIterator != controlled.messages.cend())
		{
			KTerminalMessage result;
			result.type = CommandResultTerminalMessageType;
			result.strRequestId = pendingIterator->strRequestId;
			result.strCommandId = pendingIterator->strCommandId;
			result.bSuccess = true;
			emit controlled.terminalControlMessageReceived(result);
		}
		ProcessEventsFor(10500);
		Check(pHost->nStartCount == 0
			&& std::any_of(controlled.messages.cbegin(), controlled.messages.cend(),
				[](const KTerminalMessage &message)
					{ return message.type == RejectedTerminalMessageType
						&& message.strReason == QStringLiteral("timeout"); }),
			QStringLiteral("approval timeout rejects without starting host"));

		auto spControllerHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService recoveryService(std::move(spControllerHost),
			&controller, std::move(spFrontend));
		QVector<KTerminalState> states;
		QObject::connect(&recoveryService, &KTerminalSessionService::stateChanged,
			[&states](KTerminalState state, bool, const QString &,
				const QString &, const QString &) { states.append(state); });
		controller.makeReady();
		recoveryService.openCurrentTerminal();
		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		AssignCommandId(&accepted);
		emit controller.terminalControlMessageReceived(accepted);
		emit controller.sessionStateChanged(ReconnectingSessionState);
		emit controller.sessionStateChanged(IdleSessionState);
		Check(states.contains(PausedTerminalState)
			&& states.last() == ClosedTerminalState,
			QStringLiteral("failed recovery closes the terminal"));
	}

	void TestPendingOutputOverflowFailsClosed()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller,
			std::move(spFrontend));
		QVector<KSessionError> errors;
		QObject::connect(&service, &KTerminalSessionService::structuredTerminalError,
			[&errors](const KSessionError &error) { errors.append(error); });
		controller.makeReady();
		service.openCurrentTerminal();
		const QString strRequestId = controller.messages.first().strRequestId;
		for (int nIndex = 1; nIndex <= 65; ++nIndex)
		{
			emit controller.terminalDataReceived(EncodeTerminalData(strRequestId,
				OutputTerminalDataDirection, nIndex, QByteArray(16 * 1024, 'p')));
		}
		Check(!errors.isEmpty()
			&& errors.last().code == TerminalOutputOverflowSessionErrorCode,
			QStringLiteral("pre-accept output queue overflow fails closed"));
	}

	void TestShutdownConvergesFromActiveStates()
	{
		{
			auto spHost = std::make_unique<KFakeTerminalHost>();
			KFakeSessionController controller;
			KTerminalSessionService service(std::move(spHost), &controller);
			QVector<KTerminalState> states;
			QObject::connect(&service, &KTerminalSessionService::stateChanged,
				[&states](KTerminalState state, bool, const QString &,
					const QString &, const QString &) { states.append(state); });
			service.shutdown();
			service.requestState();
			Check(states.last() == ClosedTerminalState,
				QStringLiteral("shutdown keeps an idle terminal closed"));
		}
		{
			auto spHost = std::make_unique<KFakeTerminalHost>();
			auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
			KFakeSessionController controller;
			KTerminalSessionService service(std::move(spHost), &controller,
				std::move(spFrontend));
			QVector<KTerminalState> states;
			QObject::connect(&service, &KTerminalSessionService::stateChanged,
				[&states](KTerminalState state, bool, const QString &,
					const QString &, const QString &) { states.append(state); });
			controller.makeReady();
			service.openCurrentTerminal();
			service.shutdown();
			Check(!states.isEmpty() && states.last() == ClosedTerminalState,
				QStringLiteral("shutdown closes a terminal awaiting remote approval"));
		}
		{
			auto spHost = std::make_unique<KFakeTerminalHost>();
			auto spFrontend = std::make_unique<KFakeTerminalFrontend>();
			KFakeSessionController controller;
			KTerminalSessionService service(std::move(spHost), &controller,
				std::move(spFrontend));
			QVector<KTerminalState> states;
			QObject::connect(&service, &KTerminalSessionService::stateChanged,
				[&states](KTerminalState state, bool, const QString &,
					const QString &, const QString &) { states.append(state); });
			controller.makeReady();
			service.openCurrentTerminal();
			KTerminalMessage accepted;
			accepted.type = AcceptedTerminalMessageType;
			accepted.strRequestId = controller.messages.first().strRequestId;
			AssignCommandId(&accepted);
			emit controller.terminalControlMessageReceived(accepted);
			emit controller.sessionStateChanged(ReconnectingSessionState);
			service.shutdown();
			Check(states.contains(PausedTerminalState)
				&& states.last() == ClosedTerminalState,
				QStringLiteral("shutdown closes a paused running terminal"));
		}
		{
			auto spHost = std::make_unique<KFakeTerminalHost>();
			KFakeTerminalHost *pHost = spHost.get();
			KFakeSessionController controlled;
			KTerminalSessionService service(std::move(spHost), &controlled);
			controlled.makeReady();
			KTerminalMessage request;
			request.type = OpenRequestTerminalMessageType;
			request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			request.nColumns = 100;
			request.nRows = 30;
			AssignCommandId(&request);
			emit controlled.terminalControlMessageReceived(request);
			service.shutdown();
			Check(pHost->nStopCount == 1,
				QStringLiteral("shutdown cancels a local approval without leaking host state"));
		}
	}

	void TestHostStopTimeoutConvergesAfterLateCompletion()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeTerminalHost *pHost = spHost.get();
		pHost->bEmitStopped = false;
		KFakeSessionController controlled;
		KTerminalSessionService service(std::move(spHost), &controlled);
		QVector<KTerminalState> states;
		QVector<KSessionError> errors;
		QObject::connect(&service, &KTerminalSessionService::stateChanged,
			[&states](KTerminalState state, bool, const QString &,
				const QString &, const QString &) { states.append(state); });
		QObject::connect(&service, &KTerminalSessionService::structuredTerminalError,
			[&errors](const KSessionError &error) { errors.append(error); });
		controlled.makeReady();
		KTerminalMessage request;
		request.type = OpenRequestTerminalMessageType;
		request.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		request.nColumns = 100;
		request.nRows = 30;
		AssignCommandId(&request);
		emit controlled.terminalControlMessageReceived(request);
		service.respondIncomingRequest(request.strRequestId, true);
		AcknowledgeLastCommand(&controlled);
		service.closeTerminal();
		ProcessEventsFor(3200);
		Check(!states.isEmpty() && states.last() == FailedTerminalState,
			QStringLiteral("unresponsive host stop converges to Failed"));
		Check(!errors.isEmpty()
			&& errors.last().code == ShutdownTimeoutSessionErrorCode,
			QStringLiteral("host stop timeout reports a structured error"));
		const int nStartCount = pHost->nStartCount;
		service.openCurrentTerminal();
		Check(pHost->nStartCount == nStartCount,
			QStringLiteral("timed-out host cannot be reused before late completion"));
		pHost->finishStop(controlled.nGeneration);
		Check(states.last() == ClosedTerminalState,
			QStringLiteral("late stopped signal converges Failed to Closed"));
		KTerminalMessage nextRequest;
		nextRequest.type = OpenRequestTerminalMessageType;
		nextRequest.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		nextRequest.nColumns = 100;
		nextRequest.nRows = 30;
		AssignCommandId(&nextRequest);
		emit controlled.terminalControlMessageReceived(nextRequest);
		service.respondIncomingRequest(nextRequest.strRequestId, true);
		Check(pHost->nStartCount == nStartCount + 1,
			QStringLiteral("terminal can be opened after late cleanup completes"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestControllerRequestsApproval();
	TestControlledApprovalStartsSingleHost();
	TestPendingOpenHandlesEveryReadyEventOrder();
	TestOutputBeforeAcceptedIsPreserved();
	TestOldAndOutOfOrderDataIsRejected();
	TestInputBackpressureQueuesAndFlushesInOrder();
	TestRejectAndDuplicateApprovalDoNotStartHost();
	TestChannelCloseAndReconnectLifecycle();
	TestInputOverflowAndHostFailuresAreStructured();
	TestControlSendFailureIsStructured();
	TestHostStartFailureAndFrontendWriteFailureAreReported();
	TestSessionEndClearsQueuedInput();
	TestEveryOpenRequiresApproval();
	TestAllTerminalQueuesFailClosedOnOverflow();
	TestHostRuntimeAndFrontendLifecycleFailures();
	TestApprovalTimeoutAndRecoveryFailureConverge();
	TestPendingOutputOverflowFailsClosed();
	TestShutdownConvergesFromActiveStates();
	TestHostStopTimeoutConvergesAfterLateCompletion();
	return g_nFailureCount == 0 ? 0 : 1;
}
