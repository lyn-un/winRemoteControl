#include "adapters/signaling/tcpsignalingtransport.h"
#include "adapters/windows/security/windowsdeviceidentityprovider.h"
#include "core/protocol/protocolconstraints.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	bool WaitUntil(const std::function<bool()> &condition, int nTimeoutMs = 1000)
	{
		QElapsedTimer timer;
		timer.start();
		while (!condition() && timer.elapsed() < nTimeoutMs)
		{
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
			QThread::msleep(1);
		}
		return condition();
	}

	quint16 ReserveLocalPort()
	{
		QTcpServer server;
		if (!server.listen(QHostAddress::LocalHost, 0))
			return 0;
		return server.serverPort();
	}

	QString TemporaryIdentityDirectory(const QString &strRole)
	{
		return QDir(QCoreApplication::applicationDirPath()).filePath(
			QStringLiteral("schannel-test-%1-%2").arg(strRole,
				QUuid::createUuid().toString(QUuid::WithoutBraces)));
	}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	const QString strServerDirectory = TemporaryIdentityDirectory(
		QStringLiteral("server"));
	const QString strClientDirectory = TemporaryIdentityDirectory(
		QStringLiteral("client"));
	KWindowsDeviceIdentityProvider serverIdentity(strServerDirectory);
	KWindowsDeviceIdentityProvider clientIdentity(strClientDirectory);
	QString strError;
	Check(serverIdentity.initialize(&strError),
		QStringLiteral("server device certificate is available: %1").arg(strError));
	strError.clear();
	Check(clientIdentity.initialize(&strError),
		QStringLiteral("client device certificate is available: %1").arg(strError));

	KTcpSignalingTransport server;
	KTcpSignalingTransport client;
	Check(server.setIdentityProvider(&serverIdentity, &strError),
		QStringLiteral("server Schannel identity is configured"));
	Check(client.setIdentityProvider(&clientIdentity, &strError),
		QStringLiteral("client Schannel identity is configured"));
	const quint16 nPort = ReserveLocalPort();
	Check(nPort != 0 && server.startServer(nPort, &strError),
		QStringLiteral("Schannel signaling server starts: %1").arg(strError));

	QString strServerProtocolError;
	QObject::connect(&server, &KTcpSignalingTransport::signalingError,
		[&strServerProtocolError](const QString &strMessage)
		{
			strServerProtocolError = strMessage;
		});
	QTcpSocket malformedClient;
	malformedClient.connectToHost(QHostAddress::LocalHost, nPort);
	Check(malformedClient.waitForConnected(1000),
		QStringLiteral("malformed test client connects"));
	malformedClient.write(QByteArray("WRC2CLI\0", 8));
	malformedClient.write(QByteArray(300 * 1024, 'x'));
	malformedClient.flush();
	Check(WaitUntil([&strServerProtocolError]()
		{ return !strServerProtocolError.isEmpty(); }, 3000),
		QStringLiteral("oversized malformed unauthenticated TLS input is rejected"));
	malformedClient.abort();

	bool bServerSecure = false;
	bool bClientSecure = false;
	KTlsPeerIdentity serverObservedPeer;
	KTlsPeerIdentity clientObservedPeer;
	QString strClientProtocolError;
	int nReceivedMessages = 0;
	QObject::connect(&server, &KTcpSignalingTransport::secureChannelEstablished,
		[&](const KTlsPeerIdentity &peer)
		{
			bServerSecure = true;
			serverObservedPeer = peer;
		});
	QObject::connect(&client, &KTcpSignalingTransport::secureChannelEstablished,
		[&](const KTlsPeerIdentity &peer)
		{
			bClientSecure = true;
			clientObservedPeer = peer;
		});
	QObject::connect(&client, &KTcpSignalingTransport::signalingError,
		[&strClientProtocolError](const QString &strMessage)
		{
			strClientProtocolError = strMessage;
		});
	QObject::connect(&server, &KTcpSignalingTransport::messageReceived,
		[&nReceivedMessages](const QString &) { ++nReceivedMessages; });

	client.connectToHost(QStringLiteral("127.0.0.1"), nPort);
	Check(WaitUntil([&]() { return bServerSecure && bClientSecure; }, 5000),
		QStringLiteral("Schannel establishes mutual TLS after rejecting bad input"));
	Check(serverObservedPeer.isValid() && clientObservedPeer.isValid(),
		QStringLiteral("both peers receive a validated certificate identity"));
	Check(serverObservedPeer.spkiSha256 == clientIdentity.certificate().spkiSha256
		&& clientObservedPeer.spkiSha256 == serverIdentity.certificate().spkiSha256,
		QStringLiteral("mutual TLS exposes the opposite device SPKI"));

	const QByteArray exporterLabel("EXPERIMENTAL-winRemoteControl-pairing-v1");
	const QByteArray exporterContext("deterministic-pairing-context");
	QByteArray serverKeyingMaterial;
	QByteArray clientKeyingMaterial;
	QByteArray differentKeyingMaterial;
	Check(server.exportKeyingMaterial(exporterLabel, exporterContext, 32,
		&serverKeyingMaterial, &strError),
		QStringLiteral("server exports TLS keying material"));
	Check(client.exportKeyingMaterial(exporterLabel, exporterContext, 32,
		&clientKeyingMaterial, &strError),
		QStringLiteral("client exports TLS keying material"));
	Check(serverKeyingMaterial.size() == 32
		&& serverKeyingMaterial == clientKeyingMaterial,
		QStringLiteral("both TLS peers export identical pairing material"));
	Check(client.exportKeyingMaterial(exporterLabel,
		exporterContext + QByteArray("-different"), 32,
		&differentKeyingMaterial, &strError)
		&& differentKeyingMaterial != clientKeyingMaterial,
		QStringLiteral("TLS exporter binds output to its context"));
	serverKeyingMaterial.fill('\0');
	clientKeyingMaterial.fill('\0');
	differentKeyingMaterial.fill('\0');

	client.sendMessage(QStringLiteral("{\"type\":\"encrypted\"}"));
	Check(WaitUntil([&nReceivedMessages]() { return nReceivedMessages == 1; }),
		QStringLiteral("encrypted signaling payload is delivered"));
	client.sendMessage(QString(
		KProtocolConstraints::kMaximumSignalingMessageBytes + 1, QLatin1Char('x')));
	Check(!strClientProtocolError.isEmpty()
		&& strClientProtocolError.contains(QStringLiteral("size limit")),
		QStringLiteral("oversized outgoing signaling payload is rejected explicitly"));

	client.stop();
	server.stop();
	serverIdentity.deletePersistedKey(nullptr);
	clientIdentity.deletePersistedKey(nullptr);
	QDir(strServerDirectory).removeRecursively();
	QDir(strClientDirectory).removeRecursively();
	return g_nFailureCount == 0 ? 0 : 1;
}
