#include "terminal/terminalsessionservice.h"

#include "core/terminal/terminalhost.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

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
		bool start(quint64 nGeneration, int nColumns, int nRows, QString *) override
		{
			++nStartCount;
			nLastGeneration = nGeneration;
			nLastColumns = nColumns;
			nLastRows = nRows;
			return true;
		}
		bool writeInput(quint64, const QByteArray &data) override
		{
			input.append(data);
			return true;
		}
		bool resize(quint64, int nColumns, int nRows) override
		{
			nLastColumns = nColumns;
			nLastRows = nRows;
			return true;
		}
		void requestStop(quint64) override { ++nStopCount; }

		int nStartCount = 0;
		int nStopCount = 0;
		int nLastColumns = 0;
		int nLastRows = 0;
		quint64 nLastGeneration = 0;
		QByteArray input;
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
			messages.append(message);
			return true;
		}
		bool sendTerminalData(const QByteArray &data) override
		{
			output.append(data);
			return true;
		}
		bool isTerminalBackpressured() const override { return false; }
		void sendStreamConfig(const KStreamConfig &) override {}
		void handleCaptureFailure() override {}
		void applyApplicationSettings(const KApplicationSettings &) override {}
		void respondIncomingAccessRequest(const QString &, bool) override {}
		quint64 sessionGeneration() const override { return nGeneration; }
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
	};

	void TestControllerRequestsApproval()
	{
		auto spHost = std::make_unique<KFakeTerminalHost>();
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
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
		emit controller.terminalControlMessageReceived(request);
		Check(controller.messages.last().type == ApprovalPendingTerminalMessageType,
			QStringLiteral("controlled side reports approval pending"));

		service.respondIncomingRequest(request.strRequestId, true);
		Check(pHost->nStartCount == 1,
			QStringLiteral("approval starts exactly one ConPTY host"));
		Check(controller.messages.last().type == AcceptedTerminalMessageType,
			QStringLiteral("host start is acknowledged after success"));

		emit controller.terminalDataReceived(QByteArray("dir\r\n"));
		Check(pHost->input == QByteArray("dir\r\n"),
			QStringLiteral("terminal input reaches controlled host"));
	}

	void TestPendingOpenHandlesEveryReadyEventOrder()
	{
		for (int nOrder = 0; nOrder < 6; ++nOrder)
		{
			auto spHost = std::make_unique<KFakeTerminalHost>();
			KFakeSessionController controller;
			KTerminalSessionService service(std::move(spHost), &controller);
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
		KFakeSessionController controller;
		KTerminalSessionService service(std::move(spHost), &controller);
		controller.makeReady();
		QByteArray output;
		QObject::connect(&service, &KTerminalSessionService::outputReady,
			[&output](const QByteArray &data) { output.append(data); });
		service.openCurrentTerminal();
		emit controller.terminalDataReceived(QByteArray("PowerShell banner\r\n"));
		Check(output.isEmpty(),
			QStringLiteral("early terminal output waits for acceptance"));

		KTerminalMessage accepted;
		accepted.type = AcceptedTerminalMessageType;
		accepted.strRequestId = controller.messages.first().strRequestId;
		emit controller.terminalControlMessageReceived(accepted);
		Check(output == QByteArray("PowerShell banner\r\n"),
			QStringLiteral("early terminal output is flushed after acceptance"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestControllerRequestsApproval();
	TestControlledApprovalStartsSingleHost();
	TestPendingOpenHandlesEveryReadyEventOrder();
	TestOutputBeforeAcceptedIsPreserved();
	return g_nFailureCount == 0 ? 0 : 1;
}
