#include "transport/webrtc/webrtcpeer.h"
#include "transport/webrtc/webrtcdatachannel.h"

#include <api/make_ref_counted.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtCore/QVector>

#include <functional>
#include <iostream>

class KWebRtcPeerTestAccess
{
public:
	static void routeSessionMessage(KWebRtcPeer *pPeer, const QString &strMessage)
	{
		pPeer->handleSessionChannelMessage(strMessage);
	}

	static int invalidSessionMessageCount(const KWebRtcPeer *pPeer)
	{
		return pPeer->m_nInvalidSessionMessages.load();
	}
	static void setRole(KWebRtcPeer *pPeer, KSessionRole role)
	{
		pPeer->m_role = role;
	}
	static void setSessionChannel(KWebRtcPeer *pPeer,
		webrtc::scoped_refptr<webrtc::DataChannelInterface> spChannel)
	{
		pPeer->m_pSessionDataChannel->setBufferWatermarks(0, 1);
		pPeer->m_pSessionDataChannel->setChannel(std::move(spChannel));
	}
	static qsizetype cursorQueueSize(const KWebRtcPeer *pPeer)
	{
		return pPeer->m_cursorShapeMessages.size();
	}
};

namespace
{
	int g_nFailureCount = 0;

	class KFakeCursorDataChannel : public webrtc::DataChannelInterface
	{
	public:
		void RegisterObserver(webrtc::DataChannelObserver *pObserver) override { m_pObserver = pObserver; }
		void UnregisterObserver() override { m_pObserver = nullptr; }
		std::string label() const override { return "session"; }
		bool reliable() const override { return true; }
		int id() const override { return 1; }
		DataState state() const override { return kOpen; }
		uint32_t messages_sent() const override { return static_cast<uint32_t>(sent.size()); }
		uint64_t bytes_sent() const override { return 0; }
		uint32_t messages_received() const override { return 0; }
		uint64_t bytes_received() const override { return 0; }
		uint64_t buffered_amount() const override { return m_nBuffered; }
		void Close() override {}
		bool Send(const webrtc::DataBuffer &buffer) override
		{
			sent.push_back(QString::fromUtf8(reinterpret_cast<const char *>(buffer.data.data()),
				static_cast<qsizetype>(buffer.data.size())));
			m_nBuffered = bBlockOnSend ? 1 : 0;
			return true;
		}
		void drain()
		{
			m_nBuffered = 0;
			if (m_pObserver != nullptr)
				m_pObserver->OnBufferedAmountChange(1);
		}
		QVector<QString> sent;
		bool bBlockOnSend = true;
	private:
		webrtc::DataChannelObserver *m_pObserver = nullptr;
		uint64_t m_nBuffered = 0;
	};

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	bool WaitUntil(const std::function<bool()> &condition, int nTimeoutMs = 3000)
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

	class KFaultInjectableWebRtcPeer final : public KWebRtcPeer
	{
	public:
		explicit KFaultInjectableWebRtcPeer(KPeerInitializationStage failureStage)
			: m_failureStage(failureStage)
		{
		}

	protected:
		bool shouldFailInitializationAt(KPeerInitializationStage stage) const override
		{
			if (m_bFailureInjected || stage != m_failureStage)
				return false;
			m_bFailureInjected = true;
			return true;
		}

	private:
		KPeerInitializationStage m_failureStage = NoPeerInitializationStage;
		mutable bool m_bFailureInjected = false;
	};

	void TestTerminalCommandResultRouting()
	{
		KWebRtcPeer peer;
		QVector<KTerminalMessage> receivedMessages;
		int nProtocolViolationCount = 0;
		QObject::connect(&peer, &KWebRtcPeer::terminalControlMessageReceived,
			[&receivedMessages](quint64, const KTerminalMessage &message)
			{
				receivedMessages.push_back(message);
			});
		QObject::connect(&peer, &KWebRtcPeer::protocolViolation,
			[&nProtocolViolationCount](quint64, const QString &, const QString &)
			{
				++nProtocolViolationCount;
			});

		QVector<KTerminalMessage> expectedMessages;
		for (int nIndex = 0; nIndex < 3; ++nIndex)
		{
			KTerminalMessage message;
			message.type = CommandResultTerminalMessageType;
			message.strRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			message.strCommandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			message.bSuccess = true;
			expectedMessages.push_back(message);
			KWebRtcPeerTestAccess::routeSessionMessage(
				&peer, KTerminalMessageCodec::encode(message));
		}
		QCoreApplication::processEvents();

		Check(receivedMessages.size() == expectedMessages.size(),
			QStringLiteral("terminal command results reach the Session router"));
		for (int nIndex = 0;
			nIndex < receivedMessages.size() && nIndex < expectedMessages.size();
			++nIndex)
		{
			const KTerminalMessage &actual = receivedMessages.at(nIndex);
			const KTerminalMessage &expected = expectedMessages.at(nIndex);
			Check(actual.type == CommandResultTerminalMessageType,
				QStringLiteral("terminal command result type is preserved"));
			Check(actual.strRequestId == expected.strRequestId,
				QStringLiteral("terminal command result request id is preserved"));
			Check(actual.strCommandId == expected.strCommandId,
				QStringLiteral("terminal command result command id is preserved"));
		}
		Check(KWebRtcPeerTestAccess::invalidSessionMessageCount(&peer) == 0,
			QStringLiteral("terminal command results do not count as invalid Session messages"));
		Check(nProtocolViolationCount == 0,
			QStringLiteral("terminal command results do not trigger a protocol violation"));
	}

	void TestCursorShapeRouting()
	{
		KWebRtcPeer peer;
		int nReceived = 0;
		QObject::connect(&peer, &KWebRtcPeer::cursorShapeMessageReceived,
			[&](quint64, const KCursorShapeMessage &message)
			{
				if (message.isValid())
					++nReceived;
			});
		KCursorShape shape;
		shape.nSequence = 1;
		const auto messages = KCursorShapeMessageCodec::split(shape);
		const QString strMessage = KCursorShapeMessageCodec::encode(messages[0]);
		KWebRtcPeerTestAccess::routeSessionMessage(&peer, strMessage);
		Check(nReceived == 1 && KWebRtcPeerTestAccess::invalidSessionMessageCount(&peer) == 0,
			QStringLiteral("valid cursor shape reaches controller session route"));
		KWebRtcPeerTestAccess::setRole(&peer, ControlledSessionRole);
		KWebRtcPeerTestAccess::routeSessionMessage(&peer, strMessage);
		Check(nReceived == 1 && KWebRtcPeerTestAccess::invalidSessionMessageCount(&peer) == 1,
			QStringLiteral("controlled peer rejects incoming cursor shapes"));
		KWebRtcPeerTestAccess::setRole(&peer, ControllerSessionRole);
		QString strInvalid = strMessage;
		strInvalid.replace(QStringLiteral("\"visible\":true"), QStringLiteral("\"visible\":3"));
		KWebRtcPeerTestAccess::routeSessionMessage(&peer, strInvalid);
		Check(nReceived == 1 && KWebRtcPeerTestAccess::invalidSessionMessageCount(&peer) == 2,
			QStringLiteral("malformed cursor shape rejected without delivery"));
	}

	void TestCursorBackpressure()
	{
		KWebRtcPeer peer;
		KWebRtcPeerTestAccess::setRole(&peer, ControlledSessionRole);
		auto spChannel = webrtc::make_ref_counted<KFakeCursorDataChannel>();
		KWebRtcPeerTestAccess::setSessionChannel(&peer, spChannel);
		KCursorShape shape;
		shape.kind = ColorCursorShape;
		shape.nWidth = shape.nHeight = 256;
		shape.nStride = 1024;
		shape.nSequence = 1;
		shape.data = QByteArray(256 * 1024, '\xff');
		Check(peer.sendCursorShape(shape) && spChannel->sent.size() == 1,
			QStringLiteral("cursor sender stops at data-channel high watermark"));
		for (quint64 nSequence = 2; nSequence <= 50; ++nSequence)
		{
			shape.nSequence = nSequence;
			Check(peer.sendCursorShape(shape) && KWebRtcPeerTestAccess::cursorQueueSize(&peer) == 32,
				QStringLiteral("cursor sender keeps one bounded latest shape while backpressured"));
		}
		KSessionMessage command;
		command.type = DeviceInfoRequestSessionMessageType;
		Check(peer.sendSessionMessage(command) == KRemotePeerTransport::SessionMessageAccepted,
			QStringLiteral("session command accepted while cursor is blocked"));
		spChannel->drain();
		QCoreApplication::processEvents();
		Check(spChannel->sent.size() == 2 && spChannel->sent[1].contains(QStringLiteral("deviceInfoRequest")),
			QStringLiteral("session command takes priority over pending cursor chunks"));
		spChannel->bBlockOnSend = false;
		spChannel->drain();
		QCoreApplication::processEvents();
		Check(spChannel->sent.size() == 34 && KWebRtcPeerTestAccess::cursorQueueSize(&peer) == 0,
			QStringLiteral("low watermark drains exactly one complete latest shape"));
		KCursorShapeAssembler assembler;
		KCursorShape result;
		QString strError;
		for (qsizetype nIndex = 0; nIndex < spChannel->sent.size(); ++nIndex)
		{
			if (nIndex == 1)
				continue;
			KCursorShapeMessage message;
			Check(KCursorShapeMessageCodec::decode(spChannel->sent[nIndex], &message, &strError),
				QStringLiteral("queued cursor chunk remains valid"));
			assembler.accept(message, nIndex, &result, &strError);
		}
		Check(result.nSequence == 50 && result.data == shape.data,
			QStringLiteral("partial superseded shape cannot corrupt latest cursor assembly"));
		spChannel->bBlockOnSend = true;
		shape.nSequence = 51;
		peer.sendCursorShape(shape);
		peer.clearCursorShapeQueue();
		Check(KWebRtcPeerTestAccess::cursorQueueSize(&peer) == 0,
			QStringLiteral("stop discards pending cursor chunks"));
		KWebRtcPeerTestAccess::setSessionChannel(&peer, nullptr);
	}

	void TestFailureStage(KPeerInitializationStage stage,
		KSessionRole role, quint64 nGeneration)
	{
		KFaultInjectableWebRtcPeer peer(stage);
		quint64 nShutdownGeneration = 0;
		int nPeerReadyCount = 0;
		QObject::connect(&peer, &KWebRtcPeer::shutdownFinished,
			[&nShutdownGeneration](quint64 nFinishedGeneration)
			{
				nShutdownGeneration = nFinishedGeneration;
			});
		QObject::connect(&peer, &KWebRtcPeer::stateChanged,
			[&nPeerReadyCount](quint64, const QString &strState)
			{
				if (strState == QStringLiteral("PeerReady"))
					++nPeerReadyCount;
			});

		const KPeerInitializationResult failed = peer.initialize(role, nGeneration);
		Check(failed.status == RollbackPendingPeerInitializationStatus
			&& failed.stage == stage,
			QStringLiteral("stage %1 reports rollback pending")
				.arg(KPeerInitializationResult::stageName(stage)));
		Check(nPeerReadyCount == 0,
			QStringLiteral("stage %1 does not publish PeerReady on failure")
				.arg(KPeerInitializationResult::stageName(stage)));
		Check(WaitUntil([&nShutdownGeneration, nGeneration]()
			{
				return nShutdownGeneration == nGeneration;
			}),
			QStringLiteral("stage %1 completes asynchronous rollback")
				.arg(KPeerInitializationResult::stageName(stage)));

		const quint64 nRetryGeneration = nGeneration + 100;
		const KPeerInitializationResult retried = peer.initialize(role, nRetryGeneration);
		Check(retried.succeeded(),
			QStringLiteral("stage %1 can initialize after rollback")
				.arg(KPeerInitializationResult::stageName(stage)));
		Check(nPeerReadyCount == 1,
			QStringLiteral("stage %1 publishes PeerReady only after success")
				.arg(KPeerInitializationResult::stageName(stage)));
		peer.requestShutdown(nRetryGeneration);
		Check(WaitUntil([&nShutdownGeneration, nRetryGeneration]()
			{
				return nShutdownGeneration == nRetryGeneration;
			}),
			QStringLiteral("stage %1 releases successful retry")
				.arg(KPeerInitializationResult::stageName(stage)));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestTerminalCommandResultRouting();
	TestCursorShapeRouting();
	TestCursorBackpressure();
	const KPeerInitializationStage controllerStages[] = {
		ThreadsPeerInitializationStage,
		FactoryPeerInitializationStage,
		PeerConnectionPeerInitializationStage,
		InputChannelPeerInitializationStage,
		RealtimeInputChannelPeerInitializationStage,
		SessionChannelPeerInitializationStage,
		ClipboardChannelPeerInitializationStage,
		TerminalChannelPeerInitializationStage,
		RemoteVideoReceiverPeerInitializationStage
	};
	quint64 nGeneration = 1;
	for (KPeerInitializationStage stage : controllerStages)
	{
		TestFailureStage(stage, ControllerSessionRole, nGeneration);
		++nGeneration;
	}
	TestFailureStage(LocalVideoTrackPeerInitializationStage,
		ControlledSessionRole, nGeneration);

	return g_nFailureCount == 0 ? 0 : 1;
}
