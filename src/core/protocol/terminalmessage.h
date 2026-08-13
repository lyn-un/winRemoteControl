#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_TERMINALMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_TERMINALMESSAGE_H_

#include <QtCore/QString>

struct KProtocolEnvelope;

enum KTerminalMessageType
{
	InvalidTerminalMessageType,
	OpenRequestTerminalMessageType,
	ApprovalPendingTerminalMessageType,
	AcceptedTerminalMessageType,
	RejectedTerminalMessageType,
	ResizeTerminalMessageType,
	CloseTerminalMessageType,
	ExitedTerminalMessageType,
	ErrorTerminalMessageType,
	CommandResultTerminalMessageType
};

struct KTerminalMessage
{
	KTerminalMessageType type = InvalidTerminalMessageType;
	QString strRequestId;
	QString strCommandId;
	QString strReason;
	QString strErrorCode;
	int nColumns = 0;
	int nRows = 0;
	int nTimeoutSeconds = 0;
	int nExitCode = 0;
	bool bSuccess = false;
};

class KTerminalMessageCodec
{
public:
	static QString encode(const KTerminalMessage &message);
	static bool decode(const QString &strMessage,
		KTerminalMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KTerminalMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KTerminalMessageType type);
	static bool isReliableCommand(KTerminalMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_TERMINALMESSAGE_H_
