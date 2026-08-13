#include "session/capabilitysessionflow.h"
#include "session/mediasessioncontroller.h"

#include <QtCore/QCoreApplication>

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
	TestMediaFlow();
	return g_nFailureCount == 0 ? 0 : 1;
}
