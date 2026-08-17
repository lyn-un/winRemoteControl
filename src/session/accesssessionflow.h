#ifndef _WINREMOTECONTROL_SESSION_ACCESSSESSIONFLOW_H_
#define _WINREMOTECONTROL_SESSION_ACCESSSESSIONFLOW_H_

#include "core/protocol/accessmessage.h"
#include "core/settings/applicationsettings.h"
#include "session/deviceauthenticationflow.h"

#include <QtCore/QObject>

class KAccessApprovalController;
class KSignalingTransport;
class KDeviceIdentityProvider;
class KTrustedDeviceStore;

class KAccessSessionFlow final : public QObject
{
	Q_OBJECT

public:
	explicit KAccessSessionFlow(KSignalingTransport *pTransport,
		KDeviceIdentityProvider *pIdentityProvider,
		KTrustedDeviceStore *pTrustedDeviceStore,
		QObject *pParent = nullptr);
	void setApplicationSettings(const KApplicationSettings &settings);
	bool startListening(quint16 nPort, QString *pErrorMessage);
	void connectToHost(const QString &strHost, quint16 nPort);
	void disconnectPeer();
	void stop();
	void beginOutgoing(quint64 nGeneration, const QString &strDeviceName);
	void beginIncoming(const QString &strSourceAddress,
		quint64 nGeneration,
		const QString &strDeviceName);
	bool handleAccessMessage(const KAccessMessage &message, quint64 nGeneration);
	bool handleTlsPairingMessage(const KTlsPairingMessage &message, quint64 nGeneration);
	void respondPairing(const QString &strRequestId,
		bool bAccepted,
		KPermissionScopes permissions);
	void respondIncoming(const QString &strRequestId, bool bAccepted);
	void rejectIncoming(const QString &strReason, bool bNotifyRemote);
	void cancelApproval(const QString &strReason, bool bNotifyRemote,
		bool bEmitOutcome = false);
	void clearApproval(const QString &strReason);
	bool hasApproval() const;
	void sendSignalingMessage(const QString &strMessage);
	void setConnected(bool bConnected);
	bool isConnected() const;
	bool matchesEndpoint(const QString &strHost, quint16 nPort) const;
	bool hasLastEndpoint() const;
	QString lastHost() const;
	quint16 lastPort() const;
	quint16 listeningPort() const;
	KDeviceAuthenticationContext authenticationContext() const;
	void clearLastEndpoint();

signals:
	void messageReceived(const QString &strMessage);
	void stateChanged(const QString &strState);
	void signalingError(const QString &strMessage);
	void outgoingConnectionEstablished();
	void outgoingConnectionFailed(const QString &strMessage);
	void incomingConnectionEstablished(const QString &strSourceAddress,
		quint16 nSourcePort);
	void connectionLost();
	void incomingAccessObserved(const QString &strDeviceName,
		const QString &strSourceAddress);
	void incomingAccessRequest(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strSourceAddress,
		qint64 nExpiresAtMs);
	void incomingAccessRequestCleared(const QString &strRequestId,
		const QString &strReason);
	void incomingAccessAccepted();
	void incomingAccessRejected(const QString &strReason);
	void outgoingAccessAccepted();
	void outgoingAccessRejected(const QString &strReason);
	void incomingSecurityRejected(const KSecurityStatus &status);
	void outgoingSecurityRejected(const KSecurityStatus &status);
	void pairingRequested(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strLocalRole,
		const QString &strVerificationCode,
		const QString &strControllerFingerprint,
		const QString &strControlledFingerprint,
		const QString &strTlsProtocol,
		const QString &strCipherSuite,
		KPermissionScopes requestedPermissions,
		qint64 nExpiresAtMs);
	void pairingCleared(const QString &strRequestId, const QString &strReason);
	void identityAuthenticated(const KDeviceAuthenticationContext &context);

private:
	bool ensureSecureIdentity(QString *pErrorMessage);
	void sendAccessMessage(const KAccessMessage &message);
	void acceptIncoming();
	void handleApprovalTimeout(const QString &strRequestId, quint64 nGeneration);
	void handleAuthenticationSucceeded(const KDeviceAuthenticationContext &context);
	void handleAuthenticationRejected(const KSecurityStatus &status);

	KSignalingTransport *m_pTransport = nullptr;
	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	KAccessApprovalController *m_pApprovalController = nullptr;
	KDeviceAuthenticationFlow *m_pAuthenticationFlow = nullptr;
	KApplicationSettings m_settings;
	QString m_strLastHost;
	quint16 m_nLastPort = 0;
	quint16 m_nListeningPort = 0;
	bool m_bConnected = false;
	QString m_strLocalDeviceName;
};

#endif // _WINREMOTECONTROL_SESSION_ACCESSSESSIONFLOW_H_
