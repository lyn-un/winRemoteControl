#ifndef _WINREMOTECONTROL_CORE_SECURITY_SECURITYSTATUS_H_
#define _WINREMOTECONTROL_CORE_SECURITY_SECURITYSTATUS_H_

#include <QtCore/QMetaType>
#include <QtCore/QString>

enum KSecurityErrorDomain
{
	UnknownSecurityErrorDomain,
	TransportSecurityErrorDomain,
	TlsHandshakeSecurityErrorDomain,
	PeerCertificateSecurityErrorDomain,
	PairingSecurityErrorDomain,
	TrustStoreSecurityErrorDomain,
	PermissionSecurityErrorDomain
};

enum KSecurityErrorCode
{
	UnknownSecurityErrorCode,
	IdentityUnavailableSecurityErrorCode,
	AuthenticationTimeoutSecurityErrorCode,
	CertificateInvalidSecurityErrorCode,
	PairingRejectedSecurityErrorCode,
	PairingRateLimitedSecurityErrorCode,
	DeviceKeyChangedSecurityErrorCode,
	DeviceRevokedSecurityErrorCode,
	PermissionDeniedSecurityErrorCode,
	TrustStoreTamperedSecurityErrorCode,
	ChannelBindingUnavailableSecurityErrorCode,
	ProtocolIncompatibleSecurityErrorCode,
	CancelledSecurityErrorCode
};

enum KSecurityStage
{
	UnknownSecurityStage,
	ConnectSecurityStage,
	PrefaceSecurityStage,
	TlsHandshakeSecurityStage,
	PairingHelloSecurityStage,
	UserApprovalSecurityStage,
	PrepareSecurityStage,
	CommitSecurityStage,
	TrustLoadSecurityStage,
	TrustRollbackSecurityStage,
	PermissionCheckSecurityStage
};

struct KSecurityStatus
{
	KSecurityErrorDomain domain = UnknownSecurityErrorDomain;
	KSecurityErrorCode code = UnknownSecurityErrorCode;
	KSecurityStage stage = UnknownSecurityStage;
	bool bRetryable = false;
	bool bRequiresRePair = false;
	bool bUserActionRequired = false;
	QString strRequestId;
	quint64 nGeneration = 0;
	bool bOutgoing = false;
	QString strProtocolReason;
	QString strTechnicalMessage;

	bool isValid() const;
	static KSecurityStatus fromProtocolReason(const QString &strReason,
		KSecurityStage stage = UnknownSecurityStage,
		const QString &strTechnicalMessage = QString());
	static QString domainName(KSecurityErrorDomain domain);
	static QString codeName(KSecurityErrorCode code);
	static QString stageName(KSecurityStage stage);
};

Q_DECLARE_METATYPE(KSecurityStatus)

#endif // _WINREMOTECONTROL_CORE_SECURITY_SECURITYSTATUS_H_
