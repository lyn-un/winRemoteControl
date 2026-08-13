#ifndef _WINREMOTECONTROL_CORE_SESSION_SESSIONERROR_H_
#define _WINREMOTECONTROL_CORE_SESSION_SESSIONERROR_H_

#include <QtCore/QMetaType>
#include <QtCore/QString>

enum KSessionErrorDomain
{
	UnknownSessionErrorDomain = 0,
	ConfigurationSessionErrorDomain = 1,
	SignalingSessionErrorDomain = 2,
	AccessSessionErrorDomain = 3,
	ProtocolSessionErrorDomain = 4,
	WebRtcSessionErrorDomain = 5,
	CaptureSessionErrorDomain = 6,
	InputSessionErrorDomain = 7,
	ClipboardSessionErrorDomain = 8,
	ShutdownSessionErrorDomain = 9,
	TerminalSessionErrorDomain = 10
};

enum KSessionErrorCode
{
	UnknownSessionErrorCode = 0,
	InvalidArgumentSessionErrorCode = 1,
	RemoteAccessDisabledSessionErrorCode = 2,
	ConnectionFailedSessionErrorCode = 3,
	ConnectionTimeoutSessionErrorCode = 4,
	ConnectionLostSessionErrorCode = 5,
	RemoteBusySessionErrorCode = 6,
	ApprovalRejectedSessionErrorCode = 7,
	ApprovalTimeoutSessionErrorCode = 8,
	IncompatibleProtocolSessionErrorCode = 9,
	MalformedMessageSessionErrorCode = 10,
	MessageTooLargeSessionErrorCode = 11,
	ProtocolViolationSessionErrorCode = 12,
	InvalidStateSessionErrorCode = 13,
	InitializationFailedSessionErrorCode = 14,
	SendFailedSessionErrorCode = 15,
	ChannelClosedSessionErrorCode = 16,
	RecoveryFailedSessionErrorCode = 17,
	CaptureFailedSessionErrorCode = 18,
	InputBackpressureOverflowSessionErrorCode = 19,
	CommandTimeoutSessionErrorCode = 20,
	CommandQueueOverflowSessionErrorCode = 21,
	ShutdownTimeoutSessionErrorCode = 22,
	TerminalUnavailableSessionErrorCode = 23,
	TerminalApprovalRejectedSessionErrorCode = 24,
	TerminalInputOverflowSessionErrorCode = 25,
	TerminalOutputOverflowSessionErrorCode = 26,
	TerminalHostStartFailedSessionErrorCode = 27,
	TerminalRelayHandshakeFailedSessionErrorCode = 28,
	TerminalCommandTimeoutSessionErrorCode = 29
};

enum KSessionErrorStage
{
	UnknownSessionErrorStage = 0,
	StartupSessionErrorStage = 1,
	ListeningSessionErrorStage = 2,
	ConnectingSessionErrorStage = 3,
	ApprovalSessionErrorStage = 4,
	NegotiationSessionErrorStage = 5,
	ConnectedSessionErrorStage = 6,
	StreamingSessionErrorStage = 7,
	RecoverySessionErrorStage = 8,
	ShutdownSessionErrorStage = 9
};

struct KSessionError
{
	KSessionErrorDomain domain = UnknownSessionErrorDomain;
	KSessionErrorCode code = UnknownSessionErrorCode;
	KSessionErrorStage stage = UnknownSessionErrorStage;
	bool bRetryable = false;
	QString strTechnicalMessage;

	bool isValid() const;
	static QString domainName(KSessionErrorDomain domain);
	static QString codeName(KSessionErrorCode code);
	static QString stageName(KSessionErrorStage stage);
};

Q_DECLARE_METATYPE(KSessionError)

#endif // _WINREMOTECONTROL_CORE_SESSION_SESSIONERROR_H_
