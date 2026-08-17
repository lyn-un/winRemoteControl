#include "core/security/securitystatus.h"

#include <iterator>

bool KSecurityStatus::isValid() const
{
	return domain != UnknownSecurityErrorDomain
		&& code != UnknownSecurityErrorCode
		&& !strProtocolReason.isEmpty();
}

KSecurityStatus KSecurityStatus::fromProtocolReason(const QString &strReason,
	KSecurityStage stage,
	const QString &strTechnicalMessage)
{
	KSecurityStatus status;
	status.stage = stage;
	status.strProtocolReason = strReason;
	status.strTechnicalMessage = strTechnicalMessage;
	if (strReason == QStringLiteral("identity_unavailable"))
	{
		status.domain = TrustStoreSecurityErrorDomain;
		status.code = IdentityUnavailableSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("authentication_timeout"))
	{
		status.domain = PairingSecurityErrorDomain;
		status.code = AuthenticationTimeoutSecurityErrorCode;
		status.bRetryable = true;
	}
	else if (strReason == QStringLiteral("certificate_invalid"))
	{
		status.domain = PeerCertificateSecurityErrorDomain;
		status.code = CertificateInvalidSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("pairing_rejected"))
	{
		status.domain = PairingSecurityErrorDomain;
		status.code = PairingRejectedSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("pairing_rate_limited"))
	{
		status.domain = PairingSecurityErrorDomain;
		status.code = PairingRateLimitedSecurityErrorCode;
		status.bRetryable = true;
	}
	else if (strReason == QStringLiteral("device_key_changed"))
	{
		status.domain = PeerCertificateSecurityErrorDomain;
		status.code = DeviceKeyChangedSecurityErrorCode;
		status.bRequiresRePair = true;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("device_revoked"))
	{
		status.domain = TrustStoreSecurityErrorDomain;
		status.code = DeviceRevokedSecurityErrorCode;
		status.bRequiresRePair = true;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("permission_denied"))
	{
		status.domain = PermissionSecurityErrorDomain;
		status.code = PermissionDeniedSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("trust_store_tampered"))
	{
		status.domain = TrustStoreSecurityErrorDomain;
		status.code = TrustStoreTamperedSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("channel_binding_unavailable"))
	{
		status.domain = TlsHandshakeSecurityErrorDomain;
		status.code = ChannelBindingUnavailableSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("protocol_incompatible"))
	{
		status.domain = PairingSecurityErrorDomain;
		status.code = ProtocolIncompatibleSecurityErrorCode;
		status.bUserActionRequired = true;
	}
	else if (strReason == QStringLiteral("cancelled"))
	{
		status.domain = PairingSecurityErrorDomain;
		status.code = CancelledSecurityErrorCode;
		status.bRetryable = true;
	}
	return status;
}

QString KSecurityStatus::domainName(KSecurityErrorDomain domain)
{
	static const char *const names[] = {
		"unknown", "transport", "tls_handshake", "peer_certificate",
		"pairing", "trust_store", "permission"
	};
	const int nIndex = static_cast<int>(domain);
	return nIndex >= 0 && nIndex < static_cast<int>(std::size(names))
		? QString::fromLatin1(names[nIndex]) : QStringLiteral("unknown");
}

QString KSecurityStatus::codeName(KSecurityErrorCode code)
{
	static const char *const names[] = {
		"unknown", "identity_unavailable", "authentication_timeout",
		"certificate_invalid", "pairing_rejected", "pairing_rate_limited",
		"device_key_changed", "device_revoked", "permission_denied",
		"trust_store_tampered", "channel_binding_unavailable",
		"protocol_incompatible", "cancelled"
	};
	const int nIndex = static_cast<int>(code);
	return nIndex >= 0 && nIndex < static_cast<int>(std::size(names))
		? QString::fromLatin1(names[nIndex]) : QStringLiteral("unknown");
}

QString KSecurityStatus::stageName(KSecurityStage stage)
{
	static const char *const names[] = {
		"unknown", "connect", "preface", "tls_handshake", "pairing_hello",
		"user_approval", "prepare", "commit", "trust_load", "trust_rollback",
		"permission_check"
	};
	const int nIndex = static_cast<int>(stage);
	return nIndex >= 0 && nIndex < static_cast<int>(std::size(names))
		? QString::fromLatin1(names[nIndex]) : QStringLiteral("unknown");
}
