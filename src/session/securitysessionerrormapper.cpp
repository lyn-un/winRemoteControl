#include "session/securitysessionerrormapper.h"

KSessionError KSecuritySessionErrorMapper::map(const KSecurityStatus &status)
{
	KSessionError error;
	error.domain = SecuritySessionErrorDomain;
	error.code = errorCode(status.code);
	error.stage = errorStage(status.stage);
	error.bRetryable = status.bRetryable;
	error.strTechnicalMessage = QStringLiteral(
		"requestId=%1 generation=%2 securityDomain=%3 securityStage=%4 "
		"reason=%5 technical=%6")
		.arg(status.strRequestId)
		.arg(status.nGeneration)
		.arg(KSecurityStatus::domainName(status.domain),
			KSecurityStatus::stageName(status.stage),
			status.strProtocolReason, status.strTechnicalMessage);
	return error;
}

KSessionErrorCode KSecuritySessionErrorMapper::errorCode(
	KSecurityErrorCode code)
{
	switch (code)
	{
	case IdentityUnavailableSecurityErrorCode:
		return IdentityUnavailableSessionErrorCode;
	case AuthenticationTimeoutSecurityErrorCode:
		return AuthenticationTimeoutSessionErrorCode;
	case CertificateInvalidSecurityErrorCode:
		return CertificateInvalidSessionErrorCode;
	case PairingRejectedSecurityErrorCode:
		return PairingRejectedSessionErrorCode;
	case PairingRateLimitedSecurityErrorCode:
		return PairingRateLimitedSessionErrorCode;
	case DeviceKeyChangedSecurityErrorCode:
		return DeviceKeyChangedSessionErrorCode;
	case DeviceRevokedSecurityErrorCode:
		return DeviceRevokedSessionErrorCode;
	case PermissionDeniedSecurityErrorCode:
		return PermissionDeniedSessionErrorCode;
	case TrustStoreTamperedSecurityErrorCode:
		return TrustStoreTamperedSessionErrorCode;
	case ChannelBindingUnavailableSecurityErrorCode:
		return ChannelBindingUnavailableSessionErrorCode;
	case ProtocolIncompatibleSecurityErrorCode:
		return IncompatibleProtocolSessionErrorCode;
	case CancelledSecurityErrorCode:
		return ApprovalRejectedSessionErrorCode;
	case UnknownSecurityErrorCode:
	default:
		return UnknownSessionErrorCode;
	}
}

KSessionErrorStage KSecuritySessionErrorMapper::errorStage(
	KSecurityStage stage)
{
	switch (stage)
	{
	case ConnectSecurityStage:
	case PrefaceSecurityStage:
	case TlsHandshakeSecurityStage:
		return ConnectingSessionErrorStage;
	case TrustLoadSecurityStage:
	case TrustRollbackSecurityStage:
		return StartupSessionErrorStage;
	case PermissionCheckSecurityStage:
		return NegotiationSessionErrorStage;
	case PairingHelloSecurityStage:
	case UserApprovalSecurityStage:
	case PrepareSecurityStage:
	case CommitSecurityStage:
		return ApprovalSessionErrorStage;
	case UnknownSecurityStage:
	default:
		return UnknownSessionErrorStage;
	}
}
