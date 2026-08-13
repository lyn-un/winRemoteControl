#include "session/peerlifecyclecontroller.h"

#include "core/transport/remotepeertransport.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QTimer>

#include <iostream>

namespace
{
int g_nFailureCount = 0;

void Check(bool bCondition, const char *pDescription)
{
	if (bCondition)
		return;
	std::cerr << "FAILED: " << pDescription << '\n';
	++g_nFailureCount;
}

class KFakeRemotePeerTransport final : public KRemotePeerTransport
{
public:
	KPeerInitializationResult initialize(KSessionRole, quint64 nGeneration) override
	{
		m_nGeneration = nGeneration;
		return bRollback
			? KPeerInitializationResult::rollbackPending(
				FactoryPeerInitializationStage, QStringLiteral("injected"))
			: KPeerInitializationResult::success();
	}
	void requestShutdown(quint64 nGeneration) override
	{
		++nShutdownCount;
		nLastShutdownGeneration = nGeneration;
	}
	quint64 generation() const override { return m_nGeneration; }
	void createOffer() override {}
	void restartIce() override {}
	void handleSignalingMessage(const KWebRtcSignalingMessage &) override {}
	void pushVideoFrame(const KVideoFrame &) override {}
	void sendInputMessage(const KInputMessage &) override {}
	void sendClipboardMessage(const KClipboardMessage &) override {}
	KSessionMessageSendStatus sendTerminalControlMessage(
		const KTerminalMessage &) override { return SessionMessageAccepted; }
	bool sendTerminalData(const QByteArray &) override { return true; }
	bool terminalBackpressured() const override { return false; }
	KSessionMessageSendStatus sendSessionMessage(
		const KSessionMessage &) override { return SessionMessageAccepted; }
	void setInputRealtimeEnabled(bool) override {}
	void setStreamConfig(const KStreamConfig &) override {}

	bool bRollback = false;
	int nShutdownCount = 0;
	quint64 nLastShutdownGeneration = 0;
	quint64 m_nGeneration = 0;
};

void TestGenerationAndShutdown()
{
	KFakeRemotePeerTransport transport;
	KPeerLifecycleController controller(&transport);
	Check(controller.initialize(ControllerSessionRole).succeeded(),
		"first peer initialization succeeds");
	Check(controller.generation() == 1, "first generation is one");
	controller.requestShutdown(controller.generation());
	Check(transport.nShutdownCount == 1
		&& transport.nLastShutdownGeneration == 1,
		"shutdown uses active generation");
	Check(controller.initialize(ControlledSessionRole).succeeded(),
		"second peer initialization succeeds");
	Check(controller.generation() == 2, "generation increments once per initialization");
}

void TestRollbackRejectsReuseAndAcceptsLateCompletion()
{
	KFakeRemotePeerTransport transport;
	transport.bRollback = true;
	KPeerLifecycleController controller(&transport);
	int nFinishedCount = 0;
	QObject::connect(&controller, &KPeerLifecycleController::rollbackFinished,
		[&nFinishedCount](quint64, bool) { ++nFinishedCount; });
	Check(controller.initialize(ControllerSessionRole).status
		== RollbackPendingPeerInitializationStatus,
		"failed initialization enters rollback");
	Check(controller.rollbackPending(), "rollback is pending");
	Check(controller.initialize(ControllerSessionRole).status
		== RejectedPeerInitializationStatus,
		"initialization is rejected while rollback is pending");
	emit transport.shutdownFinished(controller.generation() - 1);
	Check(controller.rollbackPending(), "old generation completion is ignored");
	emit transport.shutdownFinished(controller.generation());
	Check(!controller.rollbackPending() && nFinishedCount == 1,
		"matching completion finishes rollback once");
	transport.bRollback = false;
	Check(controller.initialize(ControllerSessionRole).succeeded(),
		"peer can initialize after rollback finishes");
}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestGenerationAndShutdown();
	TestRollbackRejectsReuseAndAcceptsLateCompletion();
	return g_nFailureCount == 0 ? 0 : 1;
}
