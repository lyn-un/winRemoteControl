#include "core/session/sessionerror.h"

#include <iterator>

bool KSessionError::isValid() const
{
	return domain != UnknownSessionErrorDomain && code != UnknownSessionErrorCode;
}

QString KSessionError::domainName(KSessionErrorDomain domain)
{
	static const char *const names[] = {
		"unknown", "configuration", "signaling", "access", "protocol",
		"webrtc", "capture", "input", "clipboard", "shutdown", "terminal",
		"security"
	};
	const int nIndex = static_cast<int>(domain);
	return nIndex >= 0 && nIndex < static_cast<int>(std::size(names))
		? QString::fromLatin1(names[nIndex]) : QStringLiteral("unknown");
}

QString KSessionError::codeName(KSessionErrorCode code)
{
	static const char *const names[] = {
		"unknown", "invalid_argument", "remote_access_disabled", "connection_failed",
		"connection_timeout", "connection_lost", "remote_busy", "approval_rejected",
		"approval_timeout", "incompatible_protocol", "malformed_message", "message_too_large",
		"protocol_violation", "invalid_state", "initialization_failed", "send_failed",
		"channel_closed", "recovery_failed", "capture_failed", "input_backpressure_overflow",
		"command_timeout", "command_queue_overflow", "shutdown_timeout",
		"terminal_unavailable", "terminal_approval_rejected", "terminal_input_overflow",
		"terminal_output_overflow", "terminal_host_start_failed",
		"terminal_relay_handshake_failed", "terminal_command_timeout",
		"identity_unavailable", "authentication_timeout", "signature_invalid",
		"pairing_rejected", "pairing_rate_limited", "device_key_changed",
		"device_revoked", "permission_denied", "trust_store_tampered",
		"channel_binding_unavailable"
	};
	const int nIndex = static_cast<int>(code);
	return nIndex >= 0 && nIndex < static_cast<int>(std::size(names))
		? QString::fromLatin1(names[nIndex]) : QStringLiteral("unknown");
}

QString KSessionError::stageName(KSessionErrorStage stage)
{
	static const char *const names[] = {
		"unknown", "startup", "listening", "connecting", "approval",
		"negotiation", "connected", "streaming", "recovery", "shutdown"
	};
	const int nIndex = static_cast<int>(stage);
	return nIndex >= 0 && nIndex < static_cast<int>(std::size(names))
		? QString::fromLatin1(names[nIndex]) : QStringLiteral("unknown");
}
