#include "adapters/signaling/tcpsignalingtransport.h"
#include "adapters/windows/security/signedjsontrusteddevicestore.h"
#include "adapters/windows/security/windowsdeviceidentityprovider.h"
#include "core/protocol/protocolconstraints.h"
#include "core/protocol/tlspairingmessage.h"
#include "core/security/admissioncontroller.h"
#include "session/deviceauthenticationflow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpSocket>

#include <functional>
#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
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

	QString TemporaryIdentityDirectory(const QString &strRole)
	{
		return QDir(QCoreApplication::applicationDirPath()).filePath(
			QStringLiteral("schannel-test-%1-%2").arg(strRole,
				QUuid::createUuid().toString(QUuid::WithoutBraces)));
	}

	class KFailingSignedTrustedDeviceStore final : public KTrustedDeviceStore
	{
	public:
		explicit KFailingSignedTrustedDeviceStore(const QString &strFilePath)
			: m_store(strFilePath)
		{
		}

		void setIdentityProvider(KDeviceIdentityProvider *pProvider) override
		{
			m_store.setIdentityProvider(pProvider);
		}
		QVector<KTrustedDevice> loadDevices(QString *pErrorMessage) override
		{
			return m_store.loadDevices(pErrorMessage);
		}
		KTrustedDeviceStoreError lastLoadError() const override
		{
			return m_store.lastLoadError();
		}
		bool saveDevices(const QVector<KTrustedDevice> &devices,
			QString *pErrorMessage) override
		{
			++m_nSaveCount;
			if (m_nSaveCount == m_nFailOnSaveCall)
			{
				if (pErrorMessage != nullptr)
					*pErrorMessage = QStringLiteral("Injected real signed-store commit failure");
				return false;
			}
			return m_store.saveDevices(devices, pErrorMessage);
		}
		QString takeMigrationNotice() override
		{
			return m_store.takeMigrationNotice();
		}
		void failOnSaveCall(int nSaveCall)
		{
			m_nFailOnSaveCall = nSaveCall;
		}

	private:
		KSignedJsonTrustedDeviceStore m_store;
		int m_nSaveCount = 0;
		int m_nFailOnSaveCall = 0;
	};

	void RunCommitFailureScenario()
	{
		const QString strServerDirectory = TemporaryIdentityDirectory(
			QStringLiteral("commit-failure-server"));
		const QString strClientDirectory = TemporaryIdentityDirectory(
			QStringLiteral("commit-failure-client"));
		KWindowsDeviceIdentityProvider serverIdentity(strServerDirectory);
		KWindowsDeviceIdentityProvider clientIdentity(strClientDirectory);
		QString strError;
		if (!serverIdentity.initialize(&strError)
			|| !clientIdentity.initialize(&strError))
		{
			Check(false, QStringLiteral("commit-failure identities initialize: %1")
				.arg(strError));
			return;
		}
		KTcpSignalingTransport server;
		KTcpSignalingTransport client;
		KFailingSignedTrustedDeviceStore serverStore(
			QDir(strServerDirectory).filePath(QStringLiteral("trusted_devices.json")));
		KSignedJsonTrustedDeviceStore clientStore(
			QDir(strClientDirectory).filePath(QStringLiteral("trusted_devices.json")));
		serverStore.setIdentityProvider(&serverIdentity);
		clientStore.setIdentityProvider(&clientIdentity);
		serverStore.failOnSaveCall(2);
		KDeviceAuthenticationFlow serverAuthentication(&serverIdentity,
			&serverStore, &server);
		KDeviceAuthenticationFlow clientAuthentication(&clientIdentity,
			&clientStore, &client);
		Check(server.setIdentityProvider(&serverIdentity, &strError)
			&& client.setIdentityProvider(&clientIdentity, &strError)
			&& server.startServer(0, &strError),
			QStringLiteral("commit-failure real TLS server starts: %1").arg(strError));

		bool bServerSecure = false;
		bool bClientSecure = false;
		int nServerSuccess = 0;
		int nClientSuccess = 0;
		QString strServerRejection;
		QString strClientRejection;
		constexpr quint64 kGeneration = 73;
		const KPermissionScopes permissions = KPermissionScopes::fromInt(
			kAllPermissionScopeBits);
		QObject::connect(&server, &KTcpSignalingTransport::secureChannelEstablished,
			[&](const KTlsPeerIdentity &peer)
			{
				bServerSecure = true;
				serverAuthentication.setSecurePeerIdentity(peer);
			});
		QObject::connect(&client, &KTcpSignalingTransport::secureChannelEstablished,
			[&](const KTlsPeerIdentity &peer)
			{
				bClientSecure = true;
				clientAuthentication.setSecurePeerIdentity(peer);
			});
		QObject::connect(&serverAuthentication,
			&KDeviceAuthenticationFlow::messageReady,
			[&](const KTlsPairingMessage &message)
			{ server.sendMessage(KTlsPairingMessageCodec::encode(message)); });
		QObject::connect(&clientAuthentication,
			&KDeviceAuthenticationFlow::messageReady,
			[&](const KTlsPairingMessage &message)
			{ client.sendMessage(KTlsPairingMessageCodec::encode(message)); });
		QObject::connect(&server, &KTcpSignalingTransport::messageReceived,
			[&](const QString &text)
			{
				KTlsPairingMessage message;
				if (KTlsPairingMessageCodec::decode(text, &message, nullptr))
					serverAuthentication.handleMessage(message, kGeneration);
			});
		QObject::connect(&client, &KTcpSignalingTransport::messageReceived,
			[&](const QString &text)
			{
				KTlsPairingMessage message;
				if (KTlsPairingMessageCodec::decode(text, &message, nullptr))
					clientAuthentication.handleMessage(message, kGeneration);
			});
		auto approve = [&](KDeviceAuthenticationFlow &flow,
			const QString &strRequestId)
		{
			flow.respondPairing(strRequestId, true, permissions);
		};
		QObject::connect(&serverAuthentication,
			&KDeviceAuthenticationFlow::pairingRequested,
			[&](const QString &strRequestId, const QString &, const QString &,
				const QString &, const QString &, const QString &, const QString &,
				const QString &, KPermissionScopes, qint64)
			{ approve(serverAuthentication, strRequestId); });
		QObject::connect(&clientAuthentication,
			&KDeviceAuthenticationFlow::pairingRequested,
			[&](const QString &strRequestId, const QString &, const QString &,
				const QString &, const QString &, const QString &, const QString &,
				const QString &, KPermissionScopes, qint64)
			{ approve(clientAuthentication, strRequestId); });
		QObject::connect(&serverAuthentication,
			&KDeviceAuthenticationFlow::authenticationSucceeded,
			[&](const KDeviceAuthenticationContext &) { ++nServerSuccess; });
		QObject::connect(&clientAuthentication,
			&KDeviceAuthenticationFlow::authenticationSucceeded,
			[&](const KDeviceAuthenticationContext &) { ++nClientSuccess; });
		QObject::connect(&serverAuthentication,
			&KDeviceAuthenticationFlow::authenticationRejected,
			[&](const KSecurityStatus &status)
			{ strServerRejection = status.strProtocolReason; });
		QObject::connect(&clientAuthentication,
			&KDeviceAuthenticationFlow::authenticationRejected,
			[&](const KSecurityStatus &status)
			{ strClientRejection = status.strProtocolReason; });

		client.connectToHost(QStringLiteral("127.0.0.1"), server.listeningPort());
		Check(WaitUntil([&]() { return bServerSecure && bClientSecure; }, 5000),
			QStringLiteral("commit-failure scenario establishes real mTLS"));
		serverAuthentication.beginIncoming(QStringLiteral("127.0.0.1"),
			kGeneration, QStringLiteral("server"));
		clientAuthentication.beginOutgoing(
			QUuid::createUuid().toString(QUuid::WithoutBraces), kGeneration,
			QStringLiteral("client"), permissions);
		Check(WaitUntil([&]()
			{ return !strServerRejection.isEmpty() && !strClientRejection.isEmpty(); },
			5000), QStringLiteral("real signed-store commit failure rejects pairing"));
		Check(nServerSuccess == 0 && nClientSuccess == 0,
			QStringLiteral("one-sided Commit failure cannot authenticate either peer"));
		QString strLoadError;
		Check(serverStore.loadDevices(&strLoadError).isEmpty()
			&& clientStore.loadDevices(&strLoadError).isEmpty(),
			QStringLiteral("real Commit failure rolls both trust stores back"));

		client.stop();
		server.stop();
		serverIdentity.deletePersistedKey(nullptr);
		clientIdentity.deletePersistedKey(nullptr);
		QDir(strServerDirectory).removeRecursively();
		QDir(strClientDirectory).removeRecursively();
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
	KSignedJsonTrustedDeviceStore serverTrustStore(
		QDir(strServerDirectory).filePath(QStringLiteral("trusted_devices.json")));
	KSignedJsonTrustedDeviceStore clientTrustStore(
		QDir(strClientDirectory).filePath(QStringLiteral("trusted_devices.json")));
	serverTrustStore.setIdentityProvider(&serverIdentity);
	clientTrustStore.setIdentityProvider(&clientIdentity);
	KDeviceAuthenticationFlow serverAuthentication(&serverIdentity,
		&serverTrustStore, &server);
	KDeviceAuthenticationFlow clientAuthentication(&clientIdentity,
		&clientTrustStore, &client);
	Check(server.setIdentityProvider(&serverIdentity, &strError),
		QStringLiteral("server Schannel identity is configured"));
	Check(client.setIdentityProvider(&clientIdentity, &strError),
		QStringLiteral("client Schannel identity is configured"));
	Check(server.startServer(0, &strError),
		QStringLiteral("Schannel signaling server starts: %1").arg(strError));
	const quint16 nPort = server.listeningPort();
	Check(nPort != 0, QStringLiteral("Schannel server reports its assigned port"));

	QString strServerProtocolError;
	QString strServerTlsError;
	QObject::connect(&server, &KTcpSignalingTransport::signalingError,
		[&strServerProtocolError](const QString &strMessage)
		{
			strServerProtocolError = strMessage;
		});
	QObject::connect(&server, &KTcpSignalingTransport::tlsHandshakeFailed,
		[&strServerTlsError](const KSessionError &error)
		{
			strServerTlsError = error.strTechnicalMessage;
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

	QTcpSocket stalledTlsClient;
	stalledTlsClient.connectToHost(QHostAddress::LocalHost, nPort);
	Check(stalledTlsClient.waitForConnected(1000),
		QStringLiteral("stalled TLS client connects"));
	stalledTlsClient.write(QByteArray("WRC2TLS\0", 8));
	stalledTlsClient.flush();
	Check(WaitUntil([&strServerTlsError]()
		{ return strServerTlsError.contains(QStringLiteral("timed out")); }, 6500),
		QStringLiteral("real socket TLS handshake timeout is bounded"));
	stalledTlsClient.abort();
	strServerProtocolError.clear();
	strServerTlsError.clear();

	QTcpSocket truncatedTlsClient;
	truncatedTlsClient.connectToHost(QHostAddress::LocalHost, nPort);
	Check(truncatedTlsClient.waitForConnected(1000),
		QStringLiteral("truncated TLS client connects"));
	truncatedTlsClient.write(QByteArray("WRC2TLS\0", 8));
	truncatedTlsClient.flush();
	Check(WaitUntil([&]() { return truncatedTlsClient.bytesAvailable() >= 8; }, 1000)
		&& truncatedTlsClient.read(8) == QByteArray("WRC2OK\0\0", 8),
		QStringLiteral("truncated TLS client reaches handshake stage"));
	truncatedTlsClient.write(QByteArray::fromHex("1603"));
	truncatedTlsClient.flush();
	QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
	truncatedTlsClient.abort();
	Check(WaitUntil([&strServerTlsError]()
		{ return strServerTlsError.contains(QStringLiteral("truncated")); }, 2000),
		QStringLiteral("truncated real TLS handshake is diagnosed"));
	strServerTlsError.clear();

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
			serverAuthentication.setSecurePeerIdentity(peer);
		});
	QObject::connect(&client, &KTcpSignalingTransport::secureChannelEstablished,
		[&](const KTlsPeerIdentity &peer)
		{
			bClientSecure = true;
			clientObservedPeer = peer;
			clientAuthentication.setSecurePeerIdentity(peer);
		});
	QObject::connect(&client, &KTcpSignalingTransport::signalingError,
		[&strClientProtocolError](const QString &strMessage)
		{
			strClientProtocolError = strMessage;
		});
	constexpr quint64 kAuthenticationGeneration = 41;
	quint64 nAuthenticationGeneration = kAuthenticationGeneration;
	QObject::connect(&serverAuthentication,
		&KDeviceAuthenticationFlow::messageReady,
		[&server](const KTlsPairingMessage &message)
		{ server.sendMessage(KTlsPairingMessageCodec::encode(message)); });
	QObject::connect(&clientAuthentication,
		&KDeviceAuthenticationFlow::messageReady,
		[&client](const KTlsPairingMessage &message)
		{ client.sendMessage(KTlsPairingMessageCodec::encode(message)); });
	QObject::connect(&server, &KTcpSignalingTransport::messageReceived,
		[&](const QString &strMessage)
		{
			KTlsPairingMessage message;
			if (KTlsPairingMessageCodec::decode(strMessage, &message, nullptr))
				serverAuthentication.handleMessage(message, nAuthenticationGeneration);
			else
				++nReceivedMessages;
		});
	QObject::connect(&client, &KTcpSignalingTransport::messageReceived,
		[&](const QString &strMessage)
		{
			KTlsPairingMessage message;
			if (KTlsPairingMessageCodec::decode(strMessage, &message, nullptr))
				clientAuthentication.handleMessage(message, nAuthenticationGeneration);
		});

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

	QString strServerPairingCode;
	QString strClientPairingCode;
	int nPairingPrompts = 0;
	int nServerAuthenticated = 0;
	int nClientAuthenticated = 0;
	const KPermissionScopes permissions = KPermissionScopes::fromInt(
		kAllPermissionScopeBits);
	auto acceptPairing = [&](KDeviceAuthenticationFlow &flow,
		QString *pCode,
		const QString &strRequestId,
		const QString &strVerificationCode)
	{
		*pCode = strVerificationCode;
		++nPairingPrompts;
		flow.respondPairing(strRequestId, true, permissions);
	};
	QObject::connect(&serverAuthentication,
		&KDeviceAuthenticationFlow::pairingRequested,
		[&](const QString &strRequestId, const QString &, const QString &,
			const QString &strCode, const QString &, const QString &,
			const QString &, const QString &, KPermissionScopes, qint64)
		{ acceptPairing(serverAuthentication, &strServerPairingCode,
			strRequestId, strCode); });
	QObject::connect(&clientAuthentication,
		&KDeviceAuthenticationFlow::pairingRequested,
		[&](const QString &strRequestId, const QString &, const QString &,
			const QString &strCode, const QString &, const QString &,
			const QString &, const QString &, KPermissionScopes, qint64)
		{ acceptPairing(clientAuthentication, &strClientPairingCode,
			strRequestId, strCode); });
	QObject::connect(&serverAuthentication,
		&KDeviceAuthenticationFlow::authenticationSucceeded,
		[&](const KDeviceAuthenticationContext &) { ++nServerAuthenticated; });
	QObject::connect(&clientAuthentication,
		&KDeviceAuthenticationFlow::authenticationSucceeded,
		[&](const KDeviceAuthenticationContext &) { ++nClientAuthenticated; });
	const KSecurityStatus serverBeginStatus = serverAuthentication.beginIncoming(
		QStringLiteral("127.0.0.1"), nAuthenticationGeneration,
		QStringLiteral("server"));
	const KSecurityStatus clientBeginStatus = clientAuthentication.beginOutgoing(
		QUuid::createUuid().toString(QUuid::WithoutBraces),
		nAuthenticationGeneration, QStringLiteral("client"), permissions);
	Check(!serverBeginStatus.isValid() && !clientBeginStatus.isValid(),
		QStringLiteral("real TLS authentication flows start"));
	Check(WaitUntil([&]()
		{ return nServerAuthenticated == 1 && nClientAuthenticated == 1; }, 5000),
		QStringLiteral("real TLS exporter completes pairing and trust commit"));
	Check(nPairingPrompts == 2
		&& !strServerPairingCode.isEmpty()
		&& strServerPairingCode == strClientPairingCode,
		QStringLiteral("real Schannel session produces the same numeric code"));
	QString strTrustError;
	Check(serverTrustStore.loadDevices(&strTrustError).size() == 1
		&& clientTrustStore.loadDevices(&strTrustError).size() == 1,
		QStringLiteral("both signed trust stores persist the paired peer"));

	client.sendMessage(QStringLiteral("{\"type\":\"encrypted\"}"));
	Check(WaitUntil([&nReceivedMessages]() { return nReceivedMessages == 1; }),
		QStringLiteral("encrypted signaling payload is delivered"));
	client.sendMessage(QString(
		KProtocolConstraints::kMaximumSignalingMessageBytes + 1, QLatin1Char('x')));
	Check(!strClientProtocolError.isEmpty()
		&& strClientProtocolError.contains(QStringLiteral("size limit")),
		QStringLiteral("oversized outgoing signaling payload is rejected explicitly"));

	bool bServerObservedGracefulClose = false;
	QObject::connect(&server, &KTcpSignalingTransport::connectionLost,
		[&bServerObservedGracefulClose]()
		{ bServerObservedGracefulClose = true; });
	strServerProtocolError.clear();
	client.sendMessage(QStringLiteral("{\"type\":\"final-before-close\"}"));
	client.stop();
	Check(WaitUntil([&bServerObservedGracefulClose]()
		{ return bServerObservedGracefulClose; }, 2000),
		QStringLiteral("Schannel close_notify closes the peer promptly"));
	Check(nReceivedMessages == 2,
		QStringLiteral("application data preceding close_notify is delivered"));
	Check(strServerProtocolError.isEmpty(),
		QStringLiteral("Schannel close_notify is not reported as signaling failure"));
	server.stop();
	bServerSecure = false;
	bClientSecure = false;
	++nAuthenticationGeneration;
	strError.clear();
	Check(server.startServer(0, &strError),
		QStringLiteral("trusted reconnect server starts: %1").arg(strError));
	const quint16 nReconnectPort = server.listeningPort();
	client.connectToHost(QStringLiteral("127.0.0.1"), nReconnectPort);
	Check(WaitUntil([&]() { return bServerSecure && bClientSecure; }, 5000),
		QStringLiteral("trusted reconnect establishes a fresh mTLS channel"));
	const KSecurityStatus serverReconnectStatus =
		serverAuthentication.beginIncoming(QStringLiteral("127.0.0.1"),
			nAuthenticationGeneration, QStringLiteral("server"));
	const KSecurityStatus clientReconnectStatus =
		clientAuthentication.beginOutgoing(
			QUuid::createUuid().toString(QUuid::WithoutBraces),
			nAuthenticationGeneration, QStringLiteral("client"), permissions);
	Check(!serverReconnectStatus.isValid() && !clientReconnectStatus.isValid(),
		QStringLiteral("trusted authentication flows restart"));
	Check(WaitUntil([&]()
		{ return nServerAuthenticated == 2 && nClientAuthenticated == 2; }, 5000),
		QStringLiteral("trusted peers authenticate without re-pairing"));
	Check(nPairingPrompts == 2,
		QStringLiteral("trusted reconnect unexpectedly requested pairing"));

	client.stop();
	server.stop();
	QVector<KTrustedDevice> changedTrust = serverTrustStore.loadDevices(&strTrustError);
	Check(changedTrust.size() == 1,
		QStringLiteral("server trust is available for key-change test"));
	if (changedTrust.size() == 1)
	{
		changedTrust[0].spkiSha256 = QByteArray(32, '\x5a');
		changedTrust[0].strFingerprint = QStringLiteral("SHA256:%1").arg(
			QString::fromLatin1(changedTrust[0].spkiSha256.toBase64(
				QByteArray::OmitTrailingEquals)));
		Check(serverTrustStore.saveDevices(changedTrust, &strTrustError),
			QStringLiteral("signed trust store accepts controlled key-change fixture"));
	}
	bServerSecure = false;
	bClientSecure = false;
	++nAuthenticationGeneration;
	QString strServerRejection;
	QObject::connect(&serverAuthentication,
		&KDeviceAuthenticationFlow::authenticationRejected,
		[&](const KSecurityStatus &status)
		{ strServerRejection = status.strProtocolReason; });
	Check(server.startServer(0, &strError),
		QStringLiteral("key-change server starts"));
	client.connectToHost(QStringLiteral("127.0.0.1"), server.listeningPort());
	Check(WaitUntil([&]() { return bServerSecure && bClientSecure; }, 5000),
		QStringLiteral("key-change test establishes mTLS before pin check"));
	serverAuthentication.beginIncoming(QStringLiteral("127.0.0.1"),
		nAuthenticationGeneration, QStringLiteral("server"));
	clientAuthentication.beginOutgoing(
		QUuid::createUuid().toString(QUuid::WithoutBraces),
		nAuthenticationGeneration, QStringLiteral("client"), permissions);
	Check(WaitUntil([&]()
		{ return strServerRejection == QStringLiteral("device_key_changed"); }, 3000),
		QStringLiteral("real TLS peer with changed pinned SPKI is rejected"));

	client.stop();
	server.stop();
	KAdmissionController admissionController;
	server.setAdmissionController(&admissionController);
	Check(server.startServer(0, &strError),
		QStringLiteral("rate-limit server starts"));
	for (int nAttempt = 0; nAttempt < 5; ++nAttempt)
	{
		QTcpSocket attacker;
		attacker.connectToHost(QHostAddress::LocalHost, server.listeningPort());
		Check(attacker.waitForConnected(1000),
			QStringLiteral("rate-limit attacker connects"));
		attacker.write(QByteArray("INVALID!", 8));
		attacker.flush();
		WaitUntil([&]()
			{ return attacker.state() == QAbstractSocket::UnconnectedState; }, 1000);
	}
	Check(admissionController.isRateLimited(QStringLiteral("127.0.0.1")),
		QStringLiteral("real socket failures enter the shared admission window"));
	QTcpSocket limitedClient;
	limitedClient.connectToHost(QHostAddress::LocalHost, server.listeningPort());
	Check(limitedClient.waitForConnected(1000),
		QStringLiteral("rate-limited client reaches the listener"));
	Check(WaitUntil([&]() { return limitedClient.bytesAvailable() >= 8; }, 1000)
		&& limitedClient.read(8) == QByteArray("WRC2BUSY", 8),
		QStringLiteral("rate-limited source receives bounded busy preface"));
	server.stop();
	serverIdentity.deletePersistedKey(nullptr);
	clientIdentity.deletePersistedKey(nullptr);
	QDir(strServerDirectory).removeRecursively();
	QDir(strClientDirectory).removeRecursively();
	RunCommitFailureScenario();
	return g_nFailureCount == 0 ? 0 : 1;
}
