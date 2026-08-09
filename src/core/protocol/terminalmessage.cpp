#include "core/protocol/terminalmessage.h"

#include "core/protocol/protocolenvelope.h"

#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace
{
	constexpr char kOpenRequest[] = "terminalOpenRequest";
	constexpr char kApprovalPending[] = "terminalApprovalPending";
	constexpr char kAccepted[] = "terminalAccepted";
	constexpr char kRejected[] = "terminalRejected";
	constexpr char kResize[] = "terminalResize";
	constexpr char kClose[] = "terminalClose";
	constexpr char kExited[] = "terminalExited";
	constexpr char kError[] = "terminalError";
	constexpr char kColumns[] = "columns";
	constexpr char kRows[] = "rows";
	constexpr char kTimeoutSeconds[] = "timeoutSeconds";
	constexpr char kReason[] = "reason";
	constexpr char kErrorCode[] = "errorCode";
	constexpr char kExitCode[] = "exitCode";
	constexpr int kMinimumColumns = 20;
	constexpr int kMaximumColumns = 400;
	constexpr int kMinimumRows = 5;
	constexpr int kMaximumRows = 200;

	bool Fail(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	bool ReadInteger(const QJsonObject &object, const char *pName, int *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isDouble())
			return false;
		const int nValue = value.toInt();
		if (value.toDouble() != static_cast<double>(nValue))
			return false;
		*pValue = nValue;
		return true;
	}
}

QString KTerminalMessageCodec::encode(const KTerminalMessage &message)
{
	QJsonObject payload;
	if (message.type == OpenRequestTerminalMessageType
		|| message.type == ResizeTerminalMessageType)
	{
		payload.insert(QString::fromLatin1(kColumns), message.nColumns);
		payload.insert(QString::fromLatin1(kRows), message.nRows);
	}
	if (message.type == ApprovalPendingTerminalMessageType)
		payload.insert(QString::fromLatin1(kTimeoutSeconds), message.nTimeoutSeconds);
	if (message.type == RejectedTerminalMessageType
		|| message.type == CloseTerminalMessageType)
	{
		payload.insert(QString::fromLatin1(kReason), message.strReason);
	}
	if (message.type == ErrorTerminalMessageType)
		payload.insert(QString::fromLatin1(kErrorCode), message.strErrorCode);
	if (message.type == ExitedTerminalMessageType)
		payload.insert(QString::fromLatin1(kExitCode), message.nExitCode);
	return KProtocolEnvelopeCodec::encode(SessionProtocolChannel,
		typeName(message.type), message.strRequestId, 0, payload);
}

bool KTerminalMessageCodec::decode(const QString &strMessage,
	KTerminalMessage *pMessage,
	QString *pErrorMessage)
{
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	return decode(envelope, pMessage, pErrorMessage);
}

bool KTerminalMessageCodec::decode(const KProtocolEnvelope &envelope,
	KTerminalMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("Terminal message output is null"), pErrorMessage);
	*pMessage = KTerminalMessage();

	KTerminalMessage message;
	message.strRequestId = envelope.strRequestId;
	const QString strType = envelope.strType;
	for (KTerminalMessageType type : { OpenRequestTerminalMessageType,
		ApprovalPendingTerminalMessageType, AcceptedTerminalMessageType,
		RejectedTerminalMessageType, ResizeTerminalMessageType,
		CloseTerminalMessageType, ExitedTerminalMessageType, ErrorTerminalMessageType })
	{
		if (strType == typeName(type))
		{
			message.type = type;
			break;
		}
	}
	if (message.type == InvalidTerminalMessageType)
		return Fail(QStringLiteral("Unknown terminal message type"), pErrorMessage);
	if (message.strRequestId.isEmpty() || QUuid(message.strRequestId).isNull())
		return Fail(QStringLiteral("Terminal request id is invalid"), pErrorMessage);

	if (message.type == OpenRequestTerminalMessageType
		|| message.type == ResizeTerminalMessageType)
	{
		if (!ReadInteger(envelope.payload, kColumns, &message.nColumns)
			|| !ReadInteger(envelope.payload, kRows, &message.nRows)
			|| message.nColumns < kMinimumColumns || message.nColumns > kMaximumColumns
			|| message.nRows < kMinimumRows || message.nRows > kMaximumRows)
		{
			return Fail(QStringLiteral("Terminal size is invalid"), pErrorMessage);
		}
	}
	else if (message.type == ApprovalPendingTerminalMessageType)
	{
		if (!ReadInteger(envelope.payload, kTimeoutSeconds, &message.nTimeoutSeconds)
			|| message.nTimeoutSeconds < 1 || message.nTimeoutSeconds > 120)
		{
			return Fail(QStringLiteral("Terminal approval timeout is invalid"), pErrorMessage);
		}
	}
	else if (message.type == RejectedTerminalMessageType
		|| message.type == CloseTerminalMessageType)
	{
		const QJsonValue value = envelope.payload.value(QString::fromLatin1(kReason));
		if (!value.isString() || value.toString().size() > 128)
			return Fail(QStringLiteral("Terminal reason is invalid"), pErrorMessage);
		message.strReason = value.toString();
	}
	else if (message.type == ErrorTerminalMessageType)
	{
		const QJsonValue value = envelope.payload.value(QString::fromLatin1(kErrorCode));
		if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 64)
			return Fail(QStringLiteral("Terminal error code is invalid"), pErrorMessage);
		message.strErrorCode = value.toString();
	}
	else if (message.type == ExitedTerminalMessageType)
	{
		if (!ReadInteger(envelope.payload, kExitCode, &message.nExitCode))
			return Fail(QStringLiteral("Terminal exit code is invalid"), pErrorMessage);
	}

	*pMessage = message;
	return true;
}

QString KTerminalMessageCodec::typeName(KTerminalMessageType type)
{
	switch (type)
	{
	case OpenRequestTerminalMessageType: return QString::fromLatin1(kOpenRequest);
	case ApprovalPendingTerminalMessageType: return QString::fromLatin1(kApprovalPending);
	case AcceptedTerminalMessageType: return QString::fromLatin1(kAccepted);
	case RejectedTerminalMessageType: return QString::fromLatin1(kRejected);
	case ResizeTerminalMessageType: return QString::fromLatin1(kResize);
	case CloseTerminalMessageType: return QString::fromLatin1(kClose);
	case ExitedTerminalMessageType: return QString::fromLatin1(kExited);
	case ErrorTerminalMessageType: return QString::fromLatin1(kError);
	case InvalidTerminalMessageType:
	default: return QStringLiteral("invalid");
	}
}
