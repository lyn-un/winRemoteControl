#ifndef _WINREMOTECONTROL_SESSION_DEVICEAUTHENTICATIONFLOW_H_
#define _WINREMOTECONTROL_SESSION_DEVICEAUTHENTICATIONFLOW_H_

#include "core/protocol/tlspairingmessage.h"
#include "core/security/devicecertificate.h"
#include "core/security/securitystatus.h"
#include "core/security/sourcefailuretracker.h"
#include "core/security/trusteddevice.h"
#include "core/transport/tlspeeridentity.h"
#include "session/pairingtransaction.h"
#include "session/trusteddeviceservice.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QVector>

class KDeviceIdentityProvider;
class KKeyingMaterialExporter;
class KTrustedDeviceStore;
class QTimer;

struct KDeviceAuthenticationContext
{
	QString strRequestId;
	QString strRemoteDeviceId;
	QString strRemoteDeviceName;
	QString strRemoteFingerprint;
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
		KKeyingMaterialExporter *pKeyingMaterialExporter,
		QObject *pParent = nullptr);

	void setApprovalTimeoutSeconds(int nTimeoutSeconds);
	void setSecurePeerIdentity(const KTlsPeerIdentity &peer);
	bool beginOutgoing(const QString &strRequestId,
		quint64 nGeneration,
		const QString &strDeviceName,
		KPermissionScopes requestedPermissions,
		QString *pErrorMessage);
	bool beginIncoming(const QString &strSourceAddress,
		quint64 nGeneration,
		const QString &strDeviceName,
		QString *pErrorMessage);
	bool handleMessage(const KTlsPairingMessage &message, quint64 nGeneration);
	void respondPairing(const QString &strRequestId,
		bool bAccepted,
		KPermissionScopes grantedPermissions);
	void cancel(const QString &strReason, bool bNotifyRemote,
		bool bEmitOutcome = true);
	bool isActive() const;
	bool isAuthenticated() const;
	KDeviceAuthenticationContext context() const;

signals:
	void messageReady(const KTlsPairingMessage &message);
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
	bool handleHello(const KTlsPairingMessage &message);
	bool handlePairingDecision(const KTlsPairingMessage &message);
	bool handlePrepared(const KTlsPairingMessage &message);
	bool handleCommitted(const KTlsPairingMessage &message);
	bool loadTrust(QString *pErrorMessage);
	bool initializePeerContext(QString *pErrorMessage);
	bool inspectPeerTrust(const QString &strPeerCommitId, QString *pReason);
	void sendHello();
	void beginPairingOrAutomaticDecision();
	bool createPairingVerification(QString *pErrorMessage);
	void sendPairingDecision(bool bAccepted, KPermissionScopes permissions);
	void tryPrepare();
	void tryCommit();
	void tryComplete();
	void rollbackPairing(const QString &strReason);
	QString localFingerprint() const;
	QString controllerFingerprint() const;
	QString controlledFingerprint() const;
	bool isSourceRateLimited();
	void recordSourceFailure();
	void fail(const QString &strReason, bool bNotifyRemote,
		KSecurityStage stage = UnknownSecurityStage,
		const QString &strTechnicalMessage = QString());
	void clear(const QString &strReason, bool bKeepPeerIdentity = true);

	KDeviceIdentityProvider *m_pIdentityProvider = nullptr;
	KKeyingMaterialExporter *m_pKeyingMaterialExporter = nullptr;
	KTrustedDeviceService m_trustedDeviceService;
	KPairingTransaction m_transaction;
	QTimer *m_pTimer = nullptr;
	KDeviceCertificate m_localCertificate;
	KTlsPeerIdentity m_securePeer;
	KDeviceAuthenticationContext m_context;
	KSourceFailureTracker m_sourceFailureTracker;
	QString m_strLocalDeviceName;
	QString m_strSourceAddress;
	QString m_strVerificationCode;
	quint64 m_nGeneration = 0;
	int m_nApprovalTimeoutSeconds = 30;
	bool m_bOutgoing = false;
	bool m_bActive = false;
	bool m_bAuthenticated = false;
	bool m_bPairingPromptVisible = false;
};

#endif // _WINREMOTECONTROL_SESSION_DEVICEAUTHENTICATIONFLOW_H_
