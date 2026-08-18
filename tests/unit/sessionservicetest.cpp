#include "adapters/signaling/tcpsignalingtransport.h"
#include "adapters/windows/security/windowsdeviceidentityprovider.h"
#include "core/input/inputinjectorinterface.h"
#include "core/protocol/accessmessage.h"
#include "core/session/deviceinfoprovider.h"
#include "core/transport/remotepeertransport.h"
#include "session/sessioncoordinator.h"
#include "fakesecurity.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>

#include <algorithm>
#include <memory>
#include <iostream>
#include <functional>

namespace
{
	int g_nFailureCount = 0;

	class KTestTlsIdentityProvider final : public KDeviceIdentityProvider
	{
	public:
		explicit KTestTlsIdentityProvider(const QString &strDirectory)
			: m_strDirectory(strDirectory), m_provider(strDirectory)
		{
		}

		~KTestTlsIdentityProvider() override
		{
			m_provider.deletePersistedKey(nullptr);
			QDir(m_strDirectory).removeRecursively();
		}

		bool initialize(QString *pErrorMessage) override
		{
			return m_provider.initialize(pErrorMessage);
		}
		KDeviceIdentity identity() const override { return m_provider.identity(); }
		KDeviceCertificate certificate() const override { return m_provider.certificate(); }
		void *duplicateNativeCertificate(QString *pErrorMessage) const override
		{
			return m_provider.duplicateNativeCertificate(pErrorMessage);
		}
		bool sign(const QByteArray &data, QByteArray *pSignature,
			QString *pErrorMessage) const override
		{
			return m_provider.sign(data, pSignature, pErrorMessage);
		}
		bool verify(const QByteArray &publicKey, const QByteArray &data,
			const QByteArray &signature, QString *pErrorMessage) const override
		{
			return m_provider.verify(publicKey, data, signature, pErrorMessage);
		}
		QByteArray randomBytes(int nByteCount, QString *pErrorMessage) const override
		{
			return m_provider.randomBytes(nByteCount, pErrorMessage);
		}

	private:
		QString m_strDirectory;
		KWindowsDeviceIdentityProvider m_provider;
	};

	std::unique_ptr<KDeviceIdentityProvider> MakeTlsIdentityProvider()
	{
		const QString strDirectory = QDir(QCoreApplication::applicationDirPath())
			.filePath(QStringLiteral("tls-test-identity-%1")
				.arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
		return std::make_unique<KTestTlsIdentityProvider>(strDirectory);
	}

#define MakeFakeIdentityProvider MakeTlsIdentityProvider

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
		KPeerInitializationResult initialize(KSessionRole role,
			quint64 nGeneration) override
		{
			initializedRole = role;
			m_nGeneration = nGeneration;
			++nInitializeCount;
			if (bInitializeSucceeds)
				return KPeerInitializationResult::success();

			++nShutdownCount;
			nLastShutdownGeneration = nGeneration;
			if (bCompleteShutdownImmediately)
			{
				QTimer::singleShot(0, this,
					[this, nGeneration]() { emit shutdownFinished(nGeneration); });
			}
			return KPeerInitializationResult::rollbackPending(
				FactoryPeerInitializationStage,
				QStringLiteral("injected initialization failure"));
		}

		void requestShutdown(quint64 nGeneration) override
		{
			++nShutdownCount;
			nLastShutdownGeneration = nGeneration;
			if (bCompleteShutdownImmediately)
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

		KSessionMessageSendStatus sendTerminalControlMessage(
			const KTerminalMessage &message) override
		{
			lastTerminalMessage = message;
			return SessionMessageAccepted;
		}

		bool sendTerminalData(const QByteArray &data) override
		{
			lastTerminalData = data;
			return true;
		}

		bool terminalBackpressured() const override { return false; }

		KSessionMessageSendStatus sendSessionMessage(
			const KSessionMessage &message) override
		{
			lastSentSessionMessage = message;
			sentSessionMessages.append(message);
			++nSentSessionCount;
			return sessionSendStatus;
		}

		void setStreamConfig(const KStreamConfig &config) override
		{
			lastStreamConfig = config;
			++nStreamConfigCount;
		}

		void setInputRealtimeEnabled(bool bEnabled) override
		{
			bInputRealtimeEnabled = bEnabled;
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
				QStringLiteral("input-realtime"), QStringLiteral("clipboard")
			};
			message.capabilities.bInputRealtime = true;
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

		void completeShutdown()
		{
			emit shutdownFinished(nLastShutdownGeneration);
		}

		KSessionRole initializedRole = ControllerSessionRole;
		KSessionMessage lastSentSessionMessage;
		QVector<KSessionMessage> sentSessionMessages;
		KInputMessage lastSentInputMessage;
		KClipboardMessage lastSentClipboardMessage;
		KTerminalMessage lastTerminalMessage;
		QByteArray lastTerminalData;
		KVideoFrame lastVideoFrame;
		KStreamConfig lastStreamConfig;
		QString strLastSignalingMessage;
		int nInitializeCount = 0;
		int nShutdownCount = 0;
		quint64 m_nGeneration = 0;
		quint64 nLastShutdownGeneration = 0;
		int nCreateOfferCount = 0;
		int nRestartIceCount = 0;
		int nVideoFrameCount = 0;
		int nSentInputCount = 0;
		int nSentClipboardCount = 0;
		int nSentSessionCount = 0;
		int nStreamConfigCount = 0;
		KSessionMessageSendStatus sessionSendStatus = SessionMessageAccepted;
		bool bInputRealtimeEnabled = false;
		bool bInitializeSucceeds = true;
		bool bCompleteShutdownImmediately = true;
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

	void approvePairingRequests(KSessionCoordinator &service,
		KPermissionScopes grantedPermissions = KPermissionScopes::fromInt(
			kAllPermissionScopeBits))
	{
		QObject::connect(&service, &KSessionCoordinator::pairingRequested,
			[&service, grantedPermissions](const QString &strRequestId,
				const QString &, const QString &, const QString &,
				const QString &, const QString &, const QString &,
				const QString &, KPermissionScopes permissions, qint64)
			{
				service.respondPairingRequest(strRequestId, true,
					permissions & grantedPermissions);
			});
	}

	void approveAccessRequests(KSessionCoordinator &service)
	{
		QObject::connect(&service, &KSessionCoordinator::incomingAccessRequest,
			[&service](const QString &strRequestId, const QString &,
				const QString &, qint64)
			{
				service.respondIncomingAccessRequest(strRequestId, true);
			});
	}

	void testControlledSessionWithFakeTransport()
	{
		auto spInputInjector = std::make_unique<KFakeInputInjector>();
		KFakeInputInjector *pInputInjector = spInputInjector.get();
		auto spTransport = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pTransport = spTransport.get();
		auto spSignaling = std::make_unique<KTcpSignalingTransport>();
		KTcpSignalingTransport *pSignaling = spSignaling.get();
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::move(spInputInjector),
			std::move(spTransport),
			std::move(spSignaling),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		KApplicationSettings settings;
		settings.approvalMode = AutoAcceptRemoteApprovalMode;
		service.applyApplicationSettings(settings);
		approvePairingRequests(service);
		approveAccessRequests(service);
		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		approvePairingRequests(controller);
		int nStartCaptureCount = 0;
		int nStopCaptureCount = 0;
		QVector<QPair<bool, quint16>> listeningAvailability;
		bool bNegotiating = false;
		int nSessionErrorCount = 0;
		KSessionError lastSessionError;
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
		QObject::connect(&service, &KSessionCoordinator::sessionErrorOccurred,
			[&](const KSessionError &error)
			{
				++nSessionErrorCount;
				lastSessionError = error;
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

		controller.connectSignaling(QStringLiteral("127.0.0.1"), nPort);
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
		check(pTransport->bInputRealtimeEnabled,
			QStringLiteral("capability negotiation enables realtime pointer input"));
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
		emit pSignaling->signalingError(QStringLiteral("unexpected active close"));
		check(nSessionErrorCount == 1
			&& lastSessionError.code == ConnectionFailedSessionErrorCode,
			QStringLiteral("unexpected active signaling close remains visible"));

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
		emit pSignaling->signalingError(QStringLiteral("expected remote close"));
		check(nSessionErrorCount == 1,
			QStringLiteral("expected signaling close after session end reached the UI"));

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
		controller.disconnectSession();
	}

	void testApprovalBeforeOffer()
	{
		auto spControlledPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControlledPeer = spControlledPeer.get();
		KSessionCoordinator controlled(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControlledPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		approvePairingRequests(controlled);
		approvePairingRequests(controller);

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
		pControllerPeer->openSessionChannel();
		pControllerPeer->deliverDefaultCapabilities();
		pControllerPeer->sessionSendStatus =
			KRemotePeerTransport::SessionMessageQueueOverflow;
		controller.sendStreamConfig(KStreamConfig());
		check(controllerError.code == CommandQueueOverflowSessionErrorCode
			&& !controllerError.bRetryable,
			QStringLiteral("session command queue overflow is a structured error"));
		pControllerPeer->sessionSendStatus =
			KRemotePeerTransport::SessionMessageAccepted;
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
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		KApplicationSettings settings;
		settings.approvalMode = AutoAcceptRemoteApprovalMode;
		controlled.applyApplicationSettings(settings);

		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		approvePairingRequests(controlled);
		approvePairingRequests(controller);
		approveAccessRequests(controlled);
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
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		approvePairingRequests(controlled);
		approvePairingRequests(controller);
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
			QStringLiteral("deny policy exposes a non-retryable structured error: "
				"domain=%1 code=%2 stage=%3 retryable=%4 technical=%5")
				.arg(controllerError.domain)
				.arg(controllerError.code)
				.arg(controllerError.stage)
				.arg(controllerError.bRetryable ? 1 : 0)
				.arg(controllerError.strTechnicalMessage));
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
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
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

	void testInitializationFailureRollsBackTransport()
	{
		auto spPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pPeer = spPeer.get();
		pPeer->bInitializeSucceeds = false;
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		KSessionError sessionError;
		QObject::connect(&service, &KSessionCoordinator::sessionErrorOccurred,
			[&sessionError](const KSessionError &error) { sessionError = error; });

		service.startSignalingServer(reserveLocalPort());
		check(pPeer->nInitializeCount == 1 && pPeer->nShutdownCount == 1,
			QStringLiteral("failed peer initialization requests resource rollback"));
		check(service.isIdle()
			&& sessionError.code == InitializationFailedSessionErrorCode
			&& sessionError.strTechnicalMessage.contains(QStringLiteral("stage=Factory")),
			QStringLiteral("failed initialization leaves the session idle with a structured error"));
	}

	void testInitializationRollbackRunsLatestPendingRequest()
	{
		auto spPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pPeer = spPeer.get();
		pPeer->bInitializeSucceeds = false;
		pPeer->bCompleteShutdownImmediately = false;
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		QVector<quint16> availablePorts;
		QObject::connect(&service, &KSessionCoordinator::listeningAvailabilityChanged,
			[&availablePorts](bool bAvailable, quint16 nPort)
			{
				if (bAvailable)
					availablePorts.append(nPort);
			});

		service.startSignalingServer(reserveLocalPort());
		pPeer->bInitializeSucceeds = true;
		pPeer->bCompleteShutdownImmediately = true;
		const quint16 nSupersededPort = reserveLocalPort();
		const quint16 nLatestPort = reserveLocalPort();
		service.startSignalingServer(nSupersededPort);
		service.startSignalingServer(nLatestPort);
		check(pPeer->nInitializeCount == 1,
			QStringLiteral("rollback prevents reuse before peer cleanup completes"));

		pPeer->completeShutdown();
		check(waitUntil([&availablePorts, nLatestPort]()
			{
				return availablePorts.contains(nLatestPort);
			}),
			QStringLiteral("rollback completion executes the latest pending listener"));
		check(!availablePorts.contains(nSupersededPort)
			&& pPeer->nInitializeCount == 2,
			QStringLiteral("superseded rollback request is not executed"));
		service.disconnectSession();
	}

	void testInitializationRollbackTimeoutAndLateCompletion()
	{
		auto spPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pPeer = spPeer.get();
		pPeer->bInitializeSucceeds = false;
		pPeer->bCompleteShutdownImmediately = false;
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		KSessionState lastState = IdleSessionState;
		KSessionError lastError;
		QObject::connect(&service, &KSessionCoordinator::sessionStateChanged,
			[&lastState](KSessionState state) { lastState = state; });
		QObject::connect(&service, &KSessionCoordinator::sessionErrorOccurred,
			[&lastError](const KSessionError &error) { lastError = error; });

		service.startSignalingServer(reserveLocalPort());
		check(waitUntil([&lastState]()
			{
				return lastState == ShutdownTimedOutSessionState;
			}, 3500),
			QStringLiteral("initialization rollback timeout isolates the peer"));
		check(lastError.code == ShutdownTimeoutSessionErrorCode,
			QStringLiteral("initialization rollback timeout reports a structured error"));
		const int nInitializeCount = pPeer->nInitializeCount;
		service.startSignalingServer(reserveLocalPort());
		check(pPeer->nInitializeCount == nInitializeCount,
			QStringLiteral("timed-out initialization resources cannot be reused"));

		pPeer->completeShutdown();
		check(waitUntil([&service]() { return service.isIdle(); }),
			QStringLiteral("late initialization rollback completion restores idle"));
	}

	void testListenerFailureRollsBackReadyPeer()
	{
		QTcpServer occupiedPort;
		check(occupiedPort.listen(QHostAddress::AnyIPv4, 0),
			QStringLiteral("listener rollback test reserves a port"));
		auto spPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pPeer = spPeer.get();
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());

		const quint16 nPort = occupiedPort.serverPort();
		service.startSignalingServer(nPort);
		check(pPeer->nInitializeCount == 1 && pPeer->nShutdownCount == 1,
			QStringLiteral("TCP listener failure rolls back the ready peer"));
		check(waitUntil([&service]() { return service.isIdle(); }),
			QStringLiteral("listener failure rollback returns to idle"));
		occupiedPort.close();
		service.startSignalingServer(nPort);
		check(pPeer->nInitializeCount == 2,
			QStringLiteral("listener can initialize again after rollback"));
		service.disconnectSession();
	}

	void testShutdownTimeoutIsolationAndLateCompletion()
	{
		auto spPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pPeer = spPeer.get();
		pPeer->bCompleteShutdownImmediately = false;
		KSessionCoordinator service(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(),
			std::move(spPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		KSessionState lastState = IdleSessionState;
		KSessionError lastError;
		QObject::connect(&service, &KSessionCoordinator::sessionStateChanged,
			[&lastState](KSessionState state) { lastState = state; });
		QObject::connect(&service, &KSessionCoordinator::sessionErrorOccurred,
			[&lastError](const KSessionError &error) { lastError = error; });

		service.startSignalingServer(reserveLocalPort());
		check(lastState == ListeningSessionState,
			QStringLiteral("timeout test starts from a listening peer"));
		service.disconnectSession();
		check(waitUntil([&lastState]()
			{
				return lastState == ShutdownTimedOutSessionState;
			}, 3500),
			QStringLiteral("missing peer completion enters shutdown timeout isolation"));
		check(lastError.domain == ShutdownSessionErrorDomain
			&& lastError.code == ShutdownTimeoutSessionErrorCode,
			QStringLiteral("shutdown timeout publishes a structured lifecycle error"));
		const int nInitializeCount = pPeer->nInitializeCount;
		service.startSignalingServer(reserveLocalPort());
		check(pPeer->nInitializeCount == nInitializeCount,
			QStringLiteral("timed-out peer cannot be reused for a new listener"));

		pPeer->completeShutdown();
		check(waitUntil([&service]() { return service.isIdle(); }),
			QStringLiteral("late peer completion finishes the isolated shutdown"));
	}

	void testViewOnlyPermissionBlocksInputAndClipboard()
	{
		auto spControlledInput = std::make_unique<KFakeInputInjector>();
		KFakeInputInjector *pControlledInput = spControlledInput.get();
		auto spControlledPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControlledPeer = spControlledPeer.get();
		KSessionCoordinator controlled(std::make_unique<KFakeDeviceInfoProvider>(),
			std::move(spControlledInput), std::move(spControlledPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		auto spControllerPeer = std::make_unique<KFakeRemotePeerTransport>();
		KFakeRemotePeerTransport *pControllerPeer = spControllerPeer.get();
		KSessionCoordinator controller(std::make_unique<KFakeDeviceInfoProvider>(),
			std::make_unique<KFakeInputInjector>(), std::move(spControllerPeer),
			std::make_unique<KTcpSignalingTransport>(),
			MakeFakeIdentityProvider(), MakeFakeTrustedDeviceStore());
		approvePairingRequests(controlled, ViewScreenPermissionScope);
		approvePairingRequests(controller);
		approveAccessRequests(controlled);

		const quint16 nPort = reserveLocalPort();
		controlled.startSignalingServer(nPort);
		controller.connectSignaling(QStringLiteral("127.0.0.1"), nPort);
		check(waitUntil([pControllerPeer]()
			{ return pControllerPeer->nCreateOfferCount == 1; }),
			QStringLiteral("view-only session completes access approval"));

		pControlledPeer->openSessionChannel();
		pControlledPeer->deliverDefaultCapabilities();
		pControlledPeer->openInputChannel();
		pControlledPeer->openClipboardChannel();
		KInputMessage key;
		key.type = KeyInputMessageType;
		key.nSequence = 1;
		key.bPressed = true;
		pControlledPeer->deliverInputMessage(key);
		KClipboardMessage clipboard;
		clipboard.type = TextClipboardMessageType;
		clipboard.strMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		clipboard.strText = QStringLiteral("blocked");
		int nClipboardReceived = 0;
		QObject::connect(&controlled, &KSessionCoordinator::clipboardMessageReceived,
			[&nClipboardReceived](const KClipboardMessage &) { ++nClipboardReceived; });
		pControlledPeer->deliverClipboardMessage(clipboard);
		check(pControlledInput->nInjectCount == 0,
			QStringLiteral("view-only permission blocks input injection"));
		check(nClipboardReceived == 0,
			QStringLiteral("view-only permission blocks clipboard delivery"));
		controller.disconnectSession();
		controlled.disconnectSession();
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
	testInitializationFailureRollsBackTransport();
	testInitializationRollbackRunsLatestPendingRequest();
	testInitializationRollbackTimeoutAndLateCompletion();
	testListenerFailureRollsBackReadyPeer();
	testShutdownTimeoutIsolationAndLateCompletion();
	testViewOnlyPermissionBlocksInputAndClipboard();
	if (g_nFailureCount == 0)
		qInfo() << "All session service tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
