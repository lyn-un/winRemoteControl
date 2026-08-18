#ifndef _WINREMOTECONTROL_SESSION_SECURITYSESSIONCONTROLLER_H_
#define _WINREMOTECONTROL_SESSION_SECURITYSESSIONCONTROLLER_H_

#include "session/deviceauthenticationflow.h"
#include "session/pairingcommand.h"

class KAdmissionController;
class KDeviceIdentityProvider;
class KSignalingTransport;
class KTrustedDeviceStore;

class KSecuritySessionController final : public QObject
{
	Q_OBJECT

public:
	KSecuritySessionController(KSignalingTransport *pTransport,
		KDeviceIdentityProvider *pIdentityProvider,
		KTrustedDeviceStore *pTrustedDeviceStore,
		QObject *pParent = nullptr);

	void setAdmissionController(KAdmissionController *pController);
	void setApprovalTimeoutSeconds(int nTimeoutSeconds);
	KSecurityStatus beginOutgoing(const QString &strRequestId,
		quint64 nGeneration,
		const QString &strDeviceName,
		KPermissionScopes requestedPermissions);
	KSecurityStatus beginIncoming(const QString &strSourceAddress,
		quint64 nGeneration,
		const QString &strDeviceName);
	bool handleMessage(const KTlsPairingMessage &message, quint64 nGeneration);
	void respondPairing(const QString &strRequestId,
		bool bAccepted,
		KPermissionScopes permissions);
	void cancel(const QString &strReason, bool bNotifyRemote,
		bool bEmitOutcome = true);
	void clearPeerIdentity();
	bool isActive() const;
	bool isAuthenticated() const;
	KDeviceAuthenticationContext context() const;

signals:
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
	void authenticationSucceeded(const KDeviceAuthenticationContext &context);
	void authenticationRejected(const KSecurityStatus &status);

private:
	KPairingCommand m_pairingCommand;
	KSignalingTransport *m_pTransport = nullptr;
	KDeviceAuthenticationFlow *m_pAuthenticationFlow = nullptr;
};

#endif // _WINREMOTECONTROL_SESSION_SECURITYSESSIONCONTROLLER_H_
