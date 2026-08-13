#include "session/accesssessionflow.h"
#include "session/capabilitysessionflow.h"
#include "session/mediasessioncontroller.h"

#include "core/transport/signalingtransport.h"

#include <QtCore/QCoreApplication>

#include <iostream>

namespace
{
int g_nFailureCount = 0;

class KFakeSignalingTransport final : public KSignalingTransport
{
public:
	bool startServer(quint16 nPort, QString *) override
	{
		nListeningPort = nPort;
		return true;
	}
	void connectToHost(const QString &strHost, quint16 nPort) override
	{
		strConnectedHost = strHost;
		nConnectedPort = nPort;
	}
	void disconnectPeer() override { ++nDisconnectCount; }
	void stop() override { ++nStopCount; }
	void setServerBusyMessage(const QString &strMessage) override
	{
		strBusyMessage = strMessage;
	}
	void sendMessage(const QString &strMessage) override
	{
		strLastMessage = strMessage;
	}

	QString strConnectedHost;
	QString strBusyMessage;
	QString strLastMessage;
	quint16 nListeningPort = 0;
	quint16 nConnectedPort = 0;
	int nDisconnectCount = 0;
	int nStopCount = 0;
};

void Check(bool bCondition, const char *pDescription)
{
	if (bCondition)
		return;
	std::cerr << "FAILED: " << pDescription << '\n';
	++g_nFailureCount;
}

KSessionCapabilities CompatibleCapabilities()
{
	KSessionCapabilities capabilities;
	capabilities.supportedCodecs = { QStringLiteral("h264") };
	capabilities.supportedChannels = {
		QStringLiteral("video"), QStringLiteral("session"), QStringLiteral("input")
	};
	return capabilities;
}

void TestCapabilityFlow()
{
	KCapabilitySessionFlow flow;
	const KSessionCapabilities local = CompatibleCapabilities();
	flow.begin(local, 1000);
	Check(!flow.isComplete(), "capability flow starts incomplete");
	const KCapabilityNegotiationResult result = flow.receive(local);
	Check(result.succeeded() && flow.isComplete(),
		"compatible peers complete capability flow");
	Check(flow.negotiatedCapabilities().strVideoCodec == QStringLiteral("h264"),
		"capability flow stores negotiated result");
	flow.reset();
	Check(!flow.isComplete(), "capability reset clears negotiated state");
}

void TestAccessFlowOwnsTransportBoundary()
{
	KFakeSignalingTransport transport;
	KAccessSessionFlow flow(&transport);
	Check(transport.strBusyMessage.contains(QStringLiteral("serverBusy")),
		"access flow configures typed server busy response");
	QString strError;
	Check(flow.startListening(39000, &strError)
		&& flow.listeningPort() == 39000,
		"access flow owns listening endpoint");
	flow.connectToHost(QStringLiteral("127.0.0.1"), 39001);
	Check(flow.matchesEndpoint(QStringLiteral("127.0.0.1"), 39001)
		&& flow.hasLastEndpoint(),
		"access flow owns the last outgoing endpoint");
	KAccessMessage request;
	request.type = RequestAccessMessageType;
	request.strRequestId = QStringLiteral("17698aa1-9108-405c-a0eb-dc1b78777ad4");
	request.strDeviceName = QStringLiteral("controller");
	flow.sendAccessMessage(request);
	Check(transport.strLastMessage.contains(QStringLiteral("accessRequest")),
		"access flow encodes business messages before transport");
}

void TestMediaFlow()
{
	KMediaSessionController media;
	KNegotiatedCapabilities capabilities;
	capabilities.bValid = true;
	capabilities.nMaximumWidth = 1280;
	capabilities.nMaximumHeight = 720;
	capabilities.nMaximumFps = 30;
	capabilities.nMaximumBitrateKbps = 4000;
	media.setCapabilities(capabilities);
	KStreamConfig requested;
	requested.nWidth = 1920;
	requested.nHeight = 1080;
	requested.nFps = 60;
	requested.nBitrateKbps = 8000;
	const KStreamConfig constrained = media.constrainedConfig(requested);
	Check(constrained.nWidth == 1280 && constrained.nHeight == 720
		&& constrained.nFps == 30 && constrained.nBitrateKbps == 4000,
		"media flow constrains stream configuration");
	int nStartCount = 0;
	int nStopCount = 0;
	QObject::connect(&media, &KMediaSessionController::startCaptureRequested,
		[&nStartCount](quint64) { ++nStartCount; });
	QObject::connect(&media, &KMediaSessionController::stopCaptureRequested,
		[&nStopCount](quint64) { ++nStopCount; });
	media.startCapture(5);
	media.startCapture(5);
	media.stopCapture(5);
	media.stopCapture(5);
	Check(nStartCount == 1 && nStopCount == 1,
		"media flow start and stop are idempotent");
}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestCapabilityFlow();
	TestAccessFlowOwnsTransportBoundary();
	TestMediaFlow();
	return g_nFailureCount == 0 ? 0 : 1;
}
