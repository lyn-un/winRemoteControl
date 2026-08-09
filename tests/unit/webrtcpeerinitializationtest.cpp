#include "transport/webrtc/webrtcpeer.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

#include <functional>
#include <iostream>

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
	const KPeerInitializationStage controllerStages[] = {
		ThreadsPeerInitializationStage,
		FactoryPeerInitializationStage,
		PeerConnectionPeerInitializationStage,
		InputChannelPeerInitializationStage,
		RealtimeInputChannelPeerInitializationStage,
		SessionChannelPeerInitializationStage,
		ClipboardChannelPeerInitializationStage,
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
