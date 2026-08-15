#ifndef _WINREMOTECONTROL_SESSION_DEVICEAUTHENTICATIONFLOW_H_
#define _WINREMOTECONTROL_SESSION_DEVICEAUTHENTICATIONFLOW_H_

#include "core/protocol/identitymessage.h"
#include "core/security/deviceidentity.h"
#include "core/security/trusteddevice.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QVector>

class KDeviceIdentityProvider;
class KTrustedDeviceStore;
class QTimer;

struct KDeviceAuthenticationContext
{
	QString strRequestId;
	QString strRemoteDeviceId;
	QString strRemoteDeviceName;
	QString strRemoteFingerprint;
	QByteArray remotePublicKey;
	QByteArray transcriptHash;
	KPermissionScopes requestedPermissions;
	KPermissionScopes effectivePermissions;
	bool bTrustedDevice = false;
	bool bRequestWithinTrust = false;
};

Q_DECLARE_METATYPE(KDeviceAuthenticationContext)

class KDeviceAuthenticationFlow final : public QObject
{
	Q_OBJECT

public:
	KDeviceAuthenticationFlow(KDeviceIdentityProvider *pIdentityProvider,
		KTrustedDeviceStore *pTrustedDeviceStore,
		QObject *pParent = nullptr);

	void setApprovalTimeoutSeconds(int nTimeoutSeconds);
	bool beginOutgoing(const QString &strRequestId,
		quint64 nGeneration,
		const QString &strDeviceName,
		KPermissionScopes requestedPermissions,
		QString *pErrorMessage);
	bool beginIncoming(const QString &strSourceAddress,
		quint64 nGeneration,
		const QString &strDeviceName,
		QString *pErrorMessage);
	bool handleMessage(const KIdentityMessage &message, quint64 nGeneration);
	void respondPairing(const QString &strRequestId,
		bool bAccepted,
		KPermissionScopes grantedPermissions);
	void cancel(const QString &strReason, bool bNotifyRemote,
		bool bEmitOutcome = true);
	bool isActive() const;
	bool isAuthenticated() const;
	KDeviceAuthenticationContext context() const;

signals:
	void messageReady(const KIdentityMessage &message);
	void pairingRequested(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strFingerprint,
		const QString &strPairingCode,
		KPermissionScopes requestedPermissions,
		qint64 nExpiresAtMs);
	void pairingCleared(const QString &strRequestId, const QString &strReason);
	void authenticationSucceeded(const KDeviceAuthenticationContext &context);
	void authenticationRejected(const QString &strReason);

private:
	bool handleHello(const KIdentityMessage &message);
	bool handleChallenge(const KIdentityMessage &message);
	bool handleProof(const KIdentityMessage &message);
	bool handlePairingDecision(const KIdentityMessage &message);
	bool handleAuthenticated(const KIdentityMessage &message);
	bool loadTrust(QString *pErrorMessage);
	bool inspectPeerTrust(QString *pReason);
	void beginPairingOrAutomaticDecision();
	void sendPairingDecision(bool bAccepted, KPermissionScopes permissions);
	void trySendAuthenticated();
	void tryComplete();
	bool persistPeerTrust(QString *pErrorMessage);
	QByteArray transcriptData() const;
	QByteArray proofData(const QString &strSenderRole) const;
	QByteArray decisionData(const QString &strSenderDeviceId,
		bool bAccepted,
		KPermissionScopes permissions) const;
	QByteArray authenticatedData(const QString &strSenderDeviceId,
		KPermissionScopes permissions) const;
	QString pairingCode() const;
	KTrustedDevice *findPeerTrust();
	const KTrustedDevice *findPeerTrust() const;
	bool isSourceRateLimited();
	void recordSourceFailure();
	void fail(const QString &strReason, bool bNotifyRemote);
	void clear(const QString &strReason);

	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	KTrustedDeviceStore *m_pTrustedDeviceStore = nullptr;
	QTimer *m_pTimer = nullptr;
	KDeviceIdentity m_localIdentity;
	KDeviceAuthenticationContext m_context;
	QVector<KTrustedDevice> m_trustedDevices;
	QHash<QString, QVector<qint64>> m_sourceFailures;
	QString m_strLocalDeviceName;
	QString m_strSourceAddress;
	QByteArray m_localNonce;
	QByteArray m_remoteNonce;
	KPermissionScopes m_localDecisionPermissions;
	KPermissionScopes m_remoteDecisionPermissions;
	quint64 m_nGeneration = 0;
	int m_nApprovalTimeoutSeconds = 30;
	bool m_bOutgoing = false;
	bool m_bActive = false;
	bool m_bAuthenticated = false;
	bool m_bRemoteProofVerified = false;
	bool m_bPairingPromptVisible = false;
	bool m_bLocalDecisionSent = false;
	bool m_bRemoteDecisionReceived = false;
	bool m_bLocalAuthenticatedSent = false;
	bool m_bRemoteAuthenticatedReceived = false;
};

#endif // _WINREMOTECONTROL_SESSION_DEVICEAUTHENTICATIONFLOW_H_
