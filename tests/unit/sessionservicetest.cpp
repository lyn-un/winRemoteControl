#include "adapters/signaling/tcpsignalingtransport.h"
#include "core/input/inputinjectorinterface.h"
#include "core/protocol/accessmessage.h"
#include "core/protocol/protocolconstraints.h"
#include "core/session/deviceinfoprovider.h"
#include "core/transport/remotepeertransport.h"
#include "session/sessioncoordinator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <memory>
#include <iostream>
#include <functional>

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
		QString deviceName() override
		{
			return QStringLiteral("fake-controlled-host");
		}

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
		bool initialize(KSessionRole role, quint64 nGeneration, QString *) override
		{
			initializedRole = role;
			m_nGeneration = nGeneration;
			++nInitializeCount;
			return true;
		}

		void requestShutdown(quint64 nGeneration) override
		{
			++nShutdownCount;
			emit shutdownFinished(nGeneration);
		}

		quint64 generation() const override { return m_nGeneration; }

		void createOffer() override
		{
			++nCreateOfferCount;
		}

		void restartIce() override
		{
			++nRestartIceCount;
		}

		void handleSignalingMessage(const KWebRtcSignalingMessage &message) override
		{
			strLastSignalingMessage = KWebRtcSignalingMessageCodec::encode(message);
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

		void sendClipboardMessage(const KClipboardMessage &message) override
		{
			lastSentClipboardMessage = message;
			++nSentClipboardCount;
		}

		bool sendSessionMessage(const KSessionMessage &message) override
		{
			lastSentSessionMessage = message;
			sentSessionMessages.append(message);
			++nSentSessionCount;
			return bSessionSendSucceeds;
		}

		void setStreamConfig(const KStreamConfig &config) override
		{
			lastStreamConfig = config;
			++nStreamConfigCount;
		}

		void openSessionChannel()
		{
			emit sessionChannelChanged(m_nGeneration, true);
		}

		void openInputChannel()
		{
			emit inputChannelChanged(m_nGeneration, true);
		}

		void openClipboardChannel()
		{
			emit clipboardChannelChanged(m_nGeneration, true);
		}

		void deliverClipboardMessage(const KClipboardMessage &message)
		{
			emit clipboardMessageReceived(m_nGeneration, message);
		}

		void deliverSessionMessage(const KSessionMessage &message)
		{
			KSessionMessage delivered = message;
			if (KSessionMessageCodec::isCommand(delivered.type)
				&& delivered.strRequestId.isEmpty())
			{
				delivered.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			}
			emit sessionMessageReceived(m_nGeneration, delivered);
		}

		void deliverDefaultCapabilities()
		{
			KSessionMessage message;
			message.type = CapabilitiesSessionMessageType;
			message.capabilities.supportedCodecs = { QStringLiteral("h264") };
			message.capabilities.supportedChannels = {
				QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input"),
				QStringLiteral("clipboard")
			};
			deliverSessionMessage(message);
		}

		void deliverInputMessage(const KInputMessage &message)
		{
			emit inputMessageReceived(m_nGeneration, message);
		}

		void interruptConnection()
		{
			emit connectionInterrupted(m_nGeneration);
		}

		void restoreConnection()
		{
			emit connectionRestored(m_nGeneration);
		}

		void terminateConnection(const QString &strReason)
		{
			emit connectionTerminated(m_nGeneration, strReason);
		}

		void overflowInputQueue()
		{
			emit inputBackpressureOverflow(m_nGeneration);
		}

		KSessionRole initializedRole = ControllerSessionRole;
		KSessionMessage lastSentSessionMessage;
		QVector<KSessionMessage> sentSessionMessages;
		KInputMessage lastSentInputMessage;
		KClipboardMessage lastSentClipboardMessage;
		KVideoFrame lastVideoFrame;
		KStreamConfig lastStreamConfig;
		QString strLastSignalingMessage;
		int nInitializeCount = 0;
		int nShutdownCount = 0;
		quint64 m_nGeneration = 0;
		int nCreateOfferCount = 0;
		int nRestartIceCount = 0;
		int nVideoFrameCount = 0;
		int nSentInputCount = 0;
		int nSentClipboardCount = 0;
		int nSentSessionCount = 0;
		int nStreamConfigCount = 0;
		bool bSessionSendSucceeds = true;
	};

	quint16 reserveLocalPort()
	{
		QTcpServer server;
		if (!server.listen(QHostAddress::LocalHost, 0))
			return 0;
		return server.serverPort();
	}

	bool waitUntil(const std::function<bool()> &condition, int nTimeoutMs = 1000)
	{
		QElapsedTimer timer;
		timer.start();
		while (!condition() && timer.elapsed() < nTimeoutMs)
		{
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
		return condition();
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
		KApplicationSettings settings;
		settings.approvalMode = AutoAcceptRemoteApprovalMode;
		service.applyApplicationSettings(settings);

		int nStartCaptureCount = 0;
		int nStopCaptureCount = 0;
		QVector<QPair<bool, quint16>> listeningAvailability;
		bool bNegotiating = false;
		QObject::connect(&service, &KSessionCoordinator::startCaptureRequested,
			[&nStartCaptureCount](quint64) { ++nStartCaptureCount; });
		QObject::connect(&service, &KSessionCoordinator::stopCaptureRequested,
			[&service, &nStopCaptureCount](quint64 nGeneration)
			{
				++nStopCaptureCount;
				service.captureShutdownFinished(nGeneration);
			});
		QObject::connect(&service, &KSessionCoordinator::sessionStateChanged,
			[&bNegotiating](KSessionState state)
			{
				if (state == NegotiatingSessionState)
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
		KAccessMessage accessRequest;
		accessRequest.type = RequestAccessMessageType;
		accessRequest.strRequestId = QStringLiteral("12345678-1234-1234-1234-1234567890ab");
		accessRequest.strDeviceName = QStringLiteral("fake-controller-host");
		controllerSocket.write(KAccessMessageCodec::encode(accessRequest).toUtf8() + '\n');
		controllerSocket.flush();
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
		pTransport->deliverDefaultCapabilities();
		pTransport->openInputChannel();

		KSessionMessage deviceInfoRequest;
		deviceInfoRequest.type = DeviceInfoRequestSessionMessageType;
		pTransport->deliverSessionMessage(deviceInfoRequest);
		const auto deviceInfoIterator = std::find_if(
			pTransport->sentSessionMessages.cbegin(), pTransport->sentSessionMessages.cend(),
			[](const KSessionMessage &message)
			{ return message.type == DeviceInfoSessionMessageType; });
		check(deviceInfoIterator != pTransport->sentSessionMessages.cend(),
			QStringLiteral("device info request is routed through transport port"));
		check(deviceInfoIterator != pTransport->sentSessionMessages.cend()
				&& deviceInfoIterator->deviceInfo.strComputerName
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

		const int nReleaseInputsBeforeRecovery = pInputInjector->nReleaseInputsCount;
		pTransport->interruptConnection();
		check(pTransport->nRestartIceCount == 0,
			QStringLiteral("controlled peer never initiates ICE restart"));
		check(pInputInjector->nReleaseInputsCount == nReleaseInputsBeforeRecovery + 1,
			QStringLiteral("controlled peer releases input when recovery begins"));
		pTransport->deliverInputMessage(keyMessage);
		service.pushVideoFrame(videoFrame);
		check(pInputInjector->nInjectCount == 1 && pTransport->nVideoFrameCount == 1,
			QStringLiteral("input injection and video sending pause during recovery"));
		pTransport->restoreConnection();
		service.pushVideoFrame(videoFrame);
		check(pTransport->nVideoFrameCount == 2,
			QStringLiteral("video sending resumes after connection recovery"));

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

	void testApprovalBeforeOffer()
	{
		auto spControlledPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControlledPeer = spControlledPeer.get();
		KSessionCoordinator controlled(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControlledPeer),
			std::make_unique<KTcpSignalingTransport>());
		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>());

		QString strRequestId;
		QString strSourceAddress;
		QString strObservedDeviceName;
		QString strObservedSourceAddress;
		KSessionError controllerError;
		QObject::connect(&controlled, &KSessionCoordinator::incomingAccessObserved,
			[&](const QString &strDeviceName, const QString &strSource)
			{
				strObservedDeviceName = strDeviceName;
				strObservedSourceAddress = strSource;
			});
		QObject::connect(&controlled, &KSessionCoordinator::incomingAccessRequest,
			[&](const QString &strId, const QString &, const QString &strSource, qint64)
			{
				strRequestId = strId;
				strSourceAddress = strSource;
			});
		QObject::connect(&controller, &KSessionCoordinator::sessionErrorOccurred,
			[&controllerError](const KSessionError &error) { controllerError = error; });

		const quint16 nPort = reserveLocalPort();
		controlled.startSignalingServer(nPort);
		controller.connectSignaling(QStringLiteral("127.0.0.1"), nPort);
		check(waitUntil([&strRequestId]() { return !strRequestId.isEmpty(); }),
			QStringLiteral("controlled host publishes an approval request"));
		check(!strSourceAddress.isEmpty(),
			QStringLiteral("approval request uses the socket source address"));
		check(strObservedDeviceName == QStringLiteral("fake-controlled-host")
			&& strObservedSourceAddress == strSourceAddress,
			QStringLiteral("incoming endpoint is published for accepted-session history"));
		check(pControllerPeer->nCreateOfferCount == 0,
			QStringLiteral("controller does not create an offer before approval"));
		check(pControlledPeer->strLastSignalingMessage.isEmpty(),
			QStringLiteral("approval messages are not forwarded into the peer"));

		controlled.respondIncomingAccessRequest(
			QStringLiteral("abcdefab-1234-5678-9abc-def012345678"), true);
		QCoreApplication::processEvents();
		check(pControllerPeer->nCreateOfferCount == 0,
			QStringLiteral("stale approval request id is ignored"));
		controlled.respondIncomingAccessRequest(strRequestId, true);
		check(waitUntil([pControllerPeer]() { return pControllerPeer->nCreateOfferCount == 1; }),
			QStringLiteral("approval creates exactly one controller offer"));
		controlled.respondIncomingAccessRequest(strRequestId, true);
		QCoreApplication::processEvents();
		check(pControllerPeer->nCreateOfferCount == 1,
			QStringLiteral("duplicate approval does not create another offer"));
		pControllerPeer->overflowInputQueue();
		QCoreApplication::processEvents();
		check(controllerError.code == InputBackpressureOverflowSessionErrorCode
			&& !controllerError.bRetryable,
			QStringLiteral("input backpressure overflow is reported without string matching"));

		controller.disconnectSession();
		controlled.disconnectSession();
	}

	void testRecoveryAndSecureRetry()
	{
		auto spControlledPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControlledPeer = spControlledPeer.get();
		KSessionCoordinator controlled(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControlledPeer),
			std::make_unique<KTcpSignalingTransport>());
		KApplicationSettings settings;
		settings.approvalMode = AutoAcceptRemoteApprovalMode;
		controlled.applyApplicationSettings(settings);

		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>());
		KSessionState controllerState = IdleSessionState;
		QString strSignalingState;
		QObject::connect(&controller, &KSessionCoordinator::sessionStateChanged,
			[&controllerState](KSessionState state) { controllerState = state; });
		QObject::connect(&controller, &KSessionCoordinator::signalingChanged,
			[&strSignalingState](const QString &strState) { strSignalingState = strState; });

		const quint16 nPort = reserveLocalPort();
		controlled.startSignalingServer(nPort);
		controller.connectSignaling(QStringLiteral("127.0.0.1"), nPort);
		check(waitUntil([pControllerPeer]() { return pControllerPeer->nCreateOfferCount == 1; }),
			QStringLiteral("automatic approval creates the initial offer"));
		pControllerPeer->openSessionChannel();
		pControllerPeer->deliverDefaultCapabilities();
		pControllerPeer->openInputChannel();
		controller.enterRemoteDesktop(KStreamConfig());

		const int nShutdownCount = pControllerPeer->nShutdownCount;
		pControllerPeer->interruptConnection();
		check(controllerState == ReconnectingSessionState,
			QStringLiteral("ICE interruption enters reconnecting state"));
		check(pControllerPeer->nRestartIceCount == 1,
			QStringLiteral("controller requests one ICE restart while signaling is available"));
		pControllerPeer->interruptConnection();
		check(pControllerPeer->nRestartIceCount == 1,
			QStringLiteral("duplicate interruption does not request another ICE restart"));
		pControllerPeer->restoreConnection();
		check(controllerState == StreamingSessionState,
			QStringLiteral("successful recovery restores streaming state"));
		check(pControllerPeer->nShutdownCount == nShutdownCount,
			QStringLiteral("successful recovery keeps the existing peer"));

		controlled.disconnectSession();
		check(waitUntil([&strSignalingState]()
			{ return strSignalingState == QStringLiteral("Disconnected"); }),
			QStringLiteral("controller observes signaling loss"));
		pControllerPeer->interruptConnection();
		check(pControllerPeer->nRestartIceCount == 1,
			QStringLiteral("ICE restart is skipped when signaling is unavailable"));
		pControllerPeer->terminateConnection(QStringLiteral("ice_failed"));
		check(controllerState == IdleSessionState,
			QStringLiteral("failed recovery ends the controller session"));

		controlled.startSignalingServer(nPort);
		controller.retryLastConnection();
		check(waitUntil([pControllerPeer]() { return pControllerPeer->nCreateOfferCount == 2; }),
			QStringLiteral("retry uses the saved endpoint and performs access approval again"));
		controller.disconnectSession();
		controlled.disconnectSession();
	}

	void testDenyPolicyRejectsBeforeOffer()
	{
		auto spControlledPeer = std::make_unique<KFakeRemotePeerTransport>();
		KSessionCoordinator controlled(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControlledPeer),
			std::make_unique<KTcpSignalingTransport>());
		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>());
		KApplicationSettings settings;
		settings.approvalMode = DenyRemoteApprovalMode;
		controlled.applyApplicationSettings(settings);
		KSessionError controllerError;
		QObject::connect(&controller, &KSessionCoordinator::sessionErrorOccurred,
			[&controllerError](const KSessionError &error) { controllerError = error; });

		const quint16 nPort = reserveLocalPort();
		controlled.startSignalingServer(nPort);
		controller.connectSignaling(QStringLiteral("127.0.0.1"), nPort);
		check(waitUntil([&controllerError]() { return controllerError.isValid(); }),
			QStringLiteral("deny policy reports rejection to controller"));
		check(controllerError.code == ApprovalRejectedSessionErrorCode
			&& !controllerError.bRetryable,
			QStringLiteral("deny policy exposes a non-retryable structured error"));
		check(pControllerPeer->nCreateOfferCount == 0,
			QStringLiteral("deny policy never creates a WebRTC offer"));
		controller.disconnectSession();
		controlled.disconnectSession();
	}

	void testDisabledRemoteAccessCannotListen()
	{
		auto spPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pPeer = spPeer.get();
		KSessionCoordinator controlled(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spPeer),
			std::make_unique<KTcpSignalingTransport>());
		KApplicationSettings settings;
		settings.bRemoteAccessEnabled = false;
		controlled.applyApplicationSettings(settings);
		KSessionError sessionError;
		QObject::connect(&controlled, &KSessionCoordinator::sessionErrorOccurred,
			[&sessionError](const KSessionError &error) { sessionError = error; });
		controlled.startSignalingServer(reserveLocalPort());
		check(sessionError.code == RemoteAccessDisabledSessionErrorCode
			&& !sessionError.bRetryable
			&& pPeer->nInitializeCount == 0,
			QStringLiteral("disabled remote access refuses to start listening"));
	}

	void testSignalingReceiveBoundaries()
	{
		KTcpSignalingTransport transport;
		QString strStartError;
		const quint16 nPort = reserveLocalPort();
		check(transport.startServer(nPort, &strStartError),
			QStringLiteral("boundary test signaling server starts"));
		bool bIncomingConnected = false;
		QString strProtocolError;
		int nReceivedMessages = 0;
		QObject::connect(&transport, &KTcpSignalingTransport::incomingConnectionEstablished,
			[&bIncomingConnected](const QString &, quint16) { bIncomingConnected = true; });
		QObject::connect(&transport, &KTcpSignalingTransport::signalingError,
			[&strProtocolError](const QString &strError) { strProtocolError = strError; });
		QObject::connect(&transport, &KTcpSignalingTransport::messageReceived,
			[&nReceivedMessages](const QString &) { ++nReceivedMessages; });

		QTcpSocket socket;
		socket.connectToHost(QHostAddress::LocalHost, nPort);
		check(socket.waitForConnected(1000)
			&& waitUntil([&bIncomingConnected]() { return bIncomingConnected; }),
			QStringLiteral("boundary test peer connects"));
		socket.write(QByteArrayLiteral("{\"type\":\"fragment"));
		socket.flush();
		QCoreApplication::processEvents();
		check(nReceivedMessages == 0,
			QStringLiteral("truncated signaling message waits for its delimiter"));
		socket.write(QByteArrayLiteral("ed\"}\n"));
		socket.flush();
		check(waitUntil([&nReceivedMessages]() { return nReceivedMessages == 1; }),
			QStringLiteral("fragmented signaling message is reassembled"));
		socket.write(QByteArrayLiteral("not-json\nnot-json\nnot-json\n"));
		socket.flush();
		check(waitUntil([&nReceivedMessages]() { return nReceivedMessages == 4; }),
			QStringLiteral("transport forwards malformed payloads to protocol router"));
		check(strProtocolError.isEmpty(),
			QStringLiteral("transport does not interpret malformed protocol payloads"));
		socket.disconnectFromHost();
		waitUntil([&socket]() { return socket.state() == QAbstractSocket::UnconnectedState; });
		transport.disconnectPeer();

		strProtocolError.clear();
		bIncomingConnected = false;
		QTcpSocket oversizedSocket;
		oversizedSocket.connectToHost(QHostAddress::LocalHost, nPort);
		check(oversizedSocket.waitForConnected(1000)
			&& waitUntil([&bIncomingConnected]() { return bIncomingConnected; }),
			QStringLiteral("oversized signaling peer connects"));
		oversizedSocket.write(QByteArray(
			KProtocolConstraints::kMaximumSignalingMessageBytes + 1, 'x'));
		oversizedSocket.flush();
		check(waitUntil([&strProtocolError]() { return !strProtocolError.isEmpty(); }),
			QStringLiteral("unterminated oversized signaling data is rejected"));
		check(strProtocolError.contains(QStringLiteral("size limit")),
			QStringLiteral("oversized signaling failure is explicit"));
		transport.stop();
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	testControlledSessionWithFakeTransport();
	testApprovalBeforeOffer();
	testRecoveryAndSecureRetry();
	testDenyPolicyRejectsBeforeOffer();
	testDisabledRemoteAccessCannotListen();
	testSignalingReceiveBoundaries();
	if (g_nFailureCount == 0)
		qInfo() << "All session service tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
