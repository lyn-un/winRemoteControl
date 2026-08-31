#include "transport/webrtc/webrtcpeer.h"

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
};

namespace
{
	int g_nFailureCount = 0;

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
