#ifndef _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_
#define _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_

#include "adapters/signaling/schanneltlsengine.h"
#include "core/transport/signalingtransport.h"

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVector>

class KDeviceIdentityProvider;
class QTcpServer;
class QTcpSocket;
class QTimer;

class KTcpSignalingTransport : public KSignalingTransport
{
	Q_OBJECT

public:
	explicit KTcpSignalingTransport(QObject *pParent = nullptr);
	~KTcpSignalingTransport() override;

	KTcpSignalingTransport(const KTcpSignalingTransport &) = delete;
	KTcpSignalingTransport &operator=(const KTcpSignalingTransport &) = delete;

	bool startServer(quint16 nPort, QString *pErrorMessage) override;
	quint16 listeningPort() const override;
	void setAdmissionController(KAdmissionController *pController) override;
	void connectToHost(const QString &strHost, quint16 nPort) override;
	void disconnectPeer() override;
	void stop() override;
	void setServerBusyMessage(const QString &strMessage) override;
	bool setIdentityProvider(KDeviceIdentityProvider *pIdentityProvider,
		QString *pErrorMessage) override;
	bool exportKeyingMaterial(const QByteArray &label,
		const QByteArray &context,
		int nLength,
		QByteArray *pKeyingMaterial,
		QString *pErrorMessage) override;
	bool isConnected() const;

public slots:
	void sendMessage(const QString &strMessage) override;

private slots:
	void handleNewConnection();
	void handleReadyRead();
	void handleConnected();
	void handleDisconnected();
	void handleSocketError();
	void handleConnectTimeout();
	void handleReadTimeout();

private:
	enum ConnectionStage
	{
		IdleStage,
		AwaitingClientPrefaceStage,
		AwaitingServerPrefaceStage,
		TlsHandshakeStage,
		SecureStage
	};

	void setSocket(QTcpSocket *pSocket);
	void closeSocket(bool bSendCloseNotify = false);
	bool writeRaw(const QByteArray &data, QString *pErrorMessage = nullptr);
	qsizetype maximumEncryptedBufferBytes() const;
	bool readAvailableData(QString *pErrorMessage);
	bool beginTls(bool bServer, QString *pErrorMessage);
	bool processPreface(QString *pErrorMessage);
	bool processTls(QString *pErrorMessage);
	bool processPlaintext(QString *pErrorMessage);
	void completeSecureConnection();
	void failOutgoingConnection(const QString &strReason, const QString &strMessage);
	void rejectPeerData(const QString &strMessage, bool bTlsFailure = false,
		bool bCountSourceFailure = false);
	bool isSourceRateLimited(const QString &strSourceAddress);
	void recordSourceFailure(const QString &strSourceAddress);

	QTcpServer *m_pServer = nullptr;
	QTcpSocket *m_pSocket = nullptr;
	QTimer *m_pConnectTimeoutTimer = nullptr;
	QTimer *m_pReadTimeoutTimer = nullptr;
	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	KSchannelTlsEngine m_tlsEngine;
	QByteArray m_encryptedBuffer;
	QByteArray m_plaintextBuffer;
	QByteArray m_serverBusyMessage;
	KAdmissionController *m_pAdmissionController = nullptr;
	KAdmissionController *m_pFallbackAdmissionController = nullptr;
	QElapsedTimer m_connectElapsedTimer;
	ConnectionStage m_stage = IdleStage;
	bool m_bOutgoing = false;
	bool m_bOutgoingConnectionPending = false;
	bool m_bPeerBusy = false;
};

#endif // _WINREMOTECONTROL_TCPSIGNALINGTRANSPORT_H_
