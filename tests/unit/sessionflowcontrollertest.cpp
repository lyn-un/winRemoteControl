#include "session/accesssessionflow.h"
#include "session/capabilitysessionflow.h"
#include "session/mediasessioncontroller.h"
#include "fakesecurity.h"

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
	bool setIdentityProvider(KDeviceIdentityProvider *, QString *) override
	{
		return true;
	}
	bool exportKeyingMaterial(const QByteArray &label,
		const QByteArray &context,
		int nLength,
		QByteArray *pKeyingMaterial,
		QString *pErrorMessage) override
	{
		return keyingMaterialExporter.exportKeyingMaterial(label, context,
			nLength, pKeyingMaterial, pErrorMessage);
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
	KFakeKeyingMaterialExporter keyingMaterialExporter;
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

KTlsPeerIdentity FakePeerIdentity(const KFakeDeviceIdentityProvider &identity)
{
	const KDeviceCertificate certificate = identity.certificate();
	KTlsPeerIdentity peer;
	peer.strDeviceId = certificate.strDeviceId;
	peer.spkiSha256 = certificate.spkiSha256;
	peer.certificateSha256 = certificate.certificateSha256;
	peer.validFromUtc = certificate.validFromUtc;
	peer.validToUtc = certificate.validToUtc;
	peer.strTlsProtocol = QStringLiteral("TLS1.3");
	peer.strCipherSuite = QStringLiteral("TLS_AES_256_GCM_SHA384");
	return peer;
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
	KFakeDeviceIdentityProvider identity;
	KFakeTrustedDeviceStore store;
	store.setIdentityProvider(&identity);
	KAccessSessionFlow flow(&transport, &identity, &store);
	Check(transport.strBusyMessage.contains(QStringLiteral("serverBusy")),
		"access flow configures typed server busy response");
	QString strError;
	Check(flow.startListening(39000, &strError)
		&& flow.listeningPort() == 39000,
		"access flow owns listening endpoint");
	flow.connectToHost(QStringLiteral("127.0.0.1"), 39001);
	emit transport.secureChannelEstablished(FakePeerIdentity(identity));
	Check(flow.matchesEndpoint(QStringLiteral("127.0.0.1"), 39001)
		&& flow.hasLastEndpoint(),
		"access flow owns the last outgoing endpoint");
	flow.beginOutgoing(7, QStringLiteral("controller"));
	KTlsPairingMessage hello;
	Check(KTlsPairingMessageCodec::decode(transport.strLastMessage, &hello, &strError)
		&& hello.type == HelloTlsPairingMessageType,
		"access flow starts identity authentication before access approval");
}

void TestAccessFlowRejectsApprovalBeforeAuthentication()
{
	KFakeSignalingTransport transport;
	KFakeDeviceIdentityProvider identity;
	KFakeTrustedDeviceStore store;
	store.setIdentityProvider(&identity);
	KAccessSessionFlow flow(&transport, &identity, &store);
	emit transport.secureChannelEstablished(FakePeerIdentity(identity));
	flow.beginOutgoing(11, QStringLiteral("controller"));
	int nOutgoingAccepted = 0;
	QObject::connect(&flow, &KAccessSessionFlow::outgoingAccessAccepted,
		[&nOutgoingAccepted]() { ++nOutgoingAccepted; });
	KAccessMessage accepted;
	accepted.type = AcceptedAccessMessageType;
	accepted.strRequestId = QStringLiteral("17698aa1-9108-405c-a0eb-dc1b78777ad4");
	Check(flow.handleAccessMessage(accepted, 11) && nOutgoingAccepted == 0,
		"access responses cannot bypass identity authentication");
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
	TestAccessFlowRejectsApprovalBeforeAuthentication();
	TestMediaFlow();
	return g_nFailureCount == 0 ? 0 : 1;
}
