#include "adapters/signaling/tcpsignalingtransport.h"
#include "core/input/inputinjectorinterface.h"
#include "core/session/deviceinfoprovider.h"
#include "core/transport/remotepeertransport.h"
#include "session/sessioncoordinator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <memory>
#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;

		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	class KFakeDeviceInfoProvider final : public IKDeviceInfoProvider
	{
	public:
		KRemoteDeviceInfo deviceInfo() override
		{
			KRemoteDeviceInfo info;
			info.strComputerName = QStringLiteral("fake-controlled-host");
			info.nScreenWidth = 1920;
			info.nScreenHeight = 1080;
			return info;
		}
	};

	class KFakeInputInjector final : public IKInputInjector
	{
	public:
		bool inject(const KInputMessage &message, QString *) override
		{
			lastMessage = message;
			++nInjectCount;
			return true;
		}

		void releaseAllKeys(QStringList *) override
		{
			++nReleaseKeysCount;
		}

		void releaseAllInputs(QStringList *) override
		{
			++nReleaseInputsCount;
		}

		KInputMessage lastMessage;
		int nInjectCount = 0;
		int nReleaseKeysCount = 0;
		int nReleaseInputsCount = 0;
	};

	class KFakeRemotePeerTransport final : public KRemotePeerTransport
	{
	public:
		bool initialize(KSessionRole role, QString *) override
		{
			initializedRole = role;
			++nInitializeCount;
			return true;
		}

		void shutdown() override
		{
			++nShutdownCount;
		}

		void createOffer() override
		{
			++nCreateOfferCount;
		}

		void handleSignalingMessage(const QString &strMessage) override
		{
			strLastSignalingMessage = strMessage;
		}

		void pushVideoFrame(const KVideoFrame &frame) override
		{
			lastVideoFrame = frame;
			++nVideoFrameCount;
		}

		void sendInputMessage(const KInputMessage &message) override
		{
			lastSentInputMessage = message;
			++nSentInputCount;
		}

		void sendSessionMessage(const KSessionMessage &message) override
		{
			lastSentSessionMessage = message;
			++nSentSessionCount;
		}

		void setStreamConfig(const KStreamConfig &config) override
		{
			lastStreamConfig = config;
			++nStreamConfigCount;
		}

		void openSessionChannel()
		{
			emit sessionChannelChanged(true);
		}

		void openInputChannel()
		{
			emit inputChannelChanged(true);
		}

		void deliverSessionMessage(const KSessionMessage &message)
		{
			emit sessionMessageReceived(message);
		}

		void deliverInputMessage(const KInputMessage &message)
		{
			emit inputMessageReceived(message);
		}

		KSessionRole initializedRole = ControllerSessionRole;
		KSessionMessage lastSentSessionMessage;
		KInputMessage lastSentInputMessage;
		KVideoFrame lastVideoFrame;
		KStreamConfig lastStreamConfig;
		QString strLastSignalingMessage;
		int nInitializeCount = 0;
		int nShutdownCount = 0;
		int nCreateOfferCount = 0;
		int nVideoFrameCount = 0;
		int nSentInputCount = 0;
		int nSentSessionCount = 0;
		int nStreamConfigCount = 0;
	};

	quint16 reserveLocalPort()
	{
		QTcpServer server;
		if (!server.listen(QHostAddress::LocalHost, 0))
			return 0;
		return server.serverPort();
	}

	void testControlledSessionWithFakeTransport()
	{
		auto spInputInjector = std::make_unique<KFakeInputInjector>();
		KFakeInputInjector *pInputInjector = spInputInjector.get();
		auto spTransport = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pTransport = spTransport.get();
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::move(spInputInjector),
			std::move(spTransport),
			std::make_unique<KTcpSignalingTransport>());

		int nStartCaptureCount = 0;
		int nStopCaptureCount = 0;
		QVector<QPair<bool, quint16>> listeningAvailability;
		bool bNegotiating = false;
		QObject::connect(&service, &KSessionCoordinator::startCaptureRequested,
			[&nStartCaptureCount]() { ++nStartCaptureCount; });
		QObject::connect(&service, &KSessionCoordinator::stopCaptureRequested,
			[&nStopCaptureCount]() { ++nStopCaptureCount; });
		QObject::connect(&service, &KSessionCoordinator::webRtcStateChanged,
			[&bNegotiating](const QString &strState)
			{
				if (strState == QStringLiteral("Negotiating"))
					bNegotiating = true;
			});
		QObject::connect(&service, &KSessionCoordinator::listeningAvailabilityChanged,
			[&](bool bAvailable, quint16 nPort)
			{
				listeningAvailability.append(qMakePair(bAvailable, nPort));
			});

		const quint16 nPort = reserveLocalPort();
		check(nPort != 0, QStringLiteral("a loopback port is available"));
		service.startSignalingServer(nPort);
		check(listeningAvailability.size() == 1
			&& listeningAvailability.first() == qMakePair(true, nPort),
			QStringLiteral("successful TCP listener becomes discoverable"));
		check(pTransport->nInitializeCount == 1,
			QStringLiteral("controlled session initializes injected transport"));
		check(pTransport->initializedRole == ControlledSessionRole,
			QStringLiteral("transport receives controlled role"));
		const int nListeningShutdownCount = pTransport->nShutdownCount;
		service.stopStreaming();
		check(pTransport->nShutdownCount == nListeningShutdownCount,
			QStringLiteral("controlled stop is ignored without an active session"));
		check(pTransport->nInitializeCount == 1,
			QStringLiteral("ignored controlled stop does not rebuild listener"));
		check(nStopCaptureCount == 0,
			QStringLiteral("ignored controlled stop does not stop capture"));

		QTcpSocket controllerSocket;
		controllerSocket.connectToHost(QHostAddress::LocalHost, nPort);
		check(controllerSocket.waitForConnected(1000),
			QStringLiteral("loopback signaling connection succeeds"));
		QElapsedTimer waitTimer;
		waitTimer.start();
		while (!bNegotiating && waitTimer.elapsed() < 1000)
		{
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
		check(bNegotiating, QStringLiteral("incoming signaling begins negotiation"));
		check(listeningAvailability.size() == 2
			&& listeningAvailability.last() == qMakePair(false, quint16(0)),
			QStringLiteral("incoming session pauses discovery availability"));
		pTransport->openSessionChannel();
		pTransport->openInputChannel();

		KSessionMessage deviceInfoRequest;
		deviceInfoRequest.type = DeviceInfoRequestSessionMessageType;
		pTransport->deliverSessionMessage(deviceInfoRequest);
		check(pTransport->lastSentSessionMessage.type == DeviceInfoSessionMessageType,
			QStringLiteral("device info request is routed through transport port"));
		check(pTransport->lastSentSessionMessage.deviceInfo.strComputerName
				== QStringLiteral("fake-controlled-host"),
			QStringLiteral("session uses injected device provider"));

		KSessionMessage startStreaming;
		startStreaming.type = StartStreamingSessionMessageType;
		pTransport->deliverSessionMessage(startStreaming);
		check(nStartCaptureCount == 1,
			QStringLiteral("start-stream message requests capture"));

		KInputMessage keyMessage;
		keyMessage.type = KeyInputMessageType;
		keyMessage.nSequence = 42;
		keyMessage.bPressed = true;
		pTransport->deliverInputMessage(keyMessage);
		check(pInputInjector->nInjectCount == 1,
			QStringLiteral("typed input reaches injected input port"));

		KVideoFrame videoFrame;
		videoFrame.nFrameIndex = 7;
		service.pushVideoFrame(videoFrame);
		check(pTransport->nVideoFrameCount == 1
				&& pTransport->lastVideoFrame.nFrameIndex == 7,
			QStringLiteral("streaming video uses generic transport port"));

		KSessionMessage stopStreaming;
		stopStreaming.type = StopStreamingSessionMessageType;
		pTransport->deliverSessionMessage(stopStreaming);
		check(nStopCaptureCount == 1,
			QStringLiteral("stop-stream message stops capture"));
		check(pInputInjector->nReleaseInputsCount > 0,
			QStringLiteral("stop-stream releases remote input state"));

		KSessionMessage endSession;
		endSession.type = EndSessionMessageType;
		endSession.strReason = QStringLiteral("test_end");
		pTransport->deliverSessionMessage(endSession);
		check(listeningAvailability.size() == 3
			&& listeningAvailability.last() == qMakePair(true, nPort),
			QStringLiteral("controlled session becomes discoverable again after session end"));

		service.disconnectSession();
		check(listeningAvailability.size() == 4
			&& listeningAvailability.last() == qMakePair(false, quint16(0)),
			QStringLiteral("fully stopping the listener disables discovery availability"));
		const int nShutdownCount = pTransport->nShutdownCount;
		const int nSentSessionCount = pTransport->nSentSessionCount;
		const int nReleaseInputsCount = pInputInjector->nReleaseInputsCount;
		const int nFinalStopCaptureCount = nStopCaptureCount;
		service.disconnectSession();
		check(pTransport->nShutdownCount == nShutdownCount,
			QStringLiteral("duplicate disconnect does not stop peer twice"));
		check(pTransport->nSentSessionCount == nSentSessionCount,
			QStringLiteral("duplicate disconnect does not notify peer twice"));
		check(pInputInjector->nReleaseInputsCount == nReleaseInputsCount,
			QStringLiteral("duplicate disconnect does not release inputs twice"));
		check(nStopCaptureCount == nFinalStopCaptureCount,
			QStringLiteral("duplicate disconnect does not stop capture twice"));
		controllerSocket.disconnectFromHost();
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	testControlledSessionWithFakeTransport();
	if (g_nFailureCount == 0)
		qInfo() << "All session service tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
