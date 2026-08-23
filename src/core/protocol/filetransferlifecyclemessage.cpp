#include "core/protocol/filetransferlifecyclemessage.h"

#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

namespace
{
	constexpr char kOpenRequest[] = "fileTransferOpenRequest";
	constexpr char kOpenAccepted[] = "fileTransferOpenAccepted";
	constexpr char kOpenRejected[] = "fileTransferOpenRejected";
	constexpr char kClose[] = "fileTransferClose";
	constexpr char kStopped[] = "fileTransferStopped";
	constexpr char kError[] = "fileTransferError";
	constexpr char kGeneration[] = "generation";
	constexpr char kErrorCode[] = "errorCode";
	constexpr int kMaximumErrorCodeCharacters = 64;

	bool Fail(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	bool ReadGeneration(const QJsonObject &object, quint64 *pGeneration)
	{
		const QJsonValue value = object.value(QString::fromLatin1(kGeneration));
		if (!value.isString())
			return false;
		const QString strValue = value.toString();
		if (strValue.isEmpty())
			return false;
		for (const QChar character : strValue)
		{
			if (!character.isDigit())
				return false;
		}
		bool bOk = false;
		const quint64 nGeneration = strValue.toULongLong(&bOk);
		if (!bOk || nGeneration == 0)
			return false;
		*pGeneration = nGeneration;
		return true;
	}

	KFileTransferLifecycleMessageType TypeFromName(const QString &strName)
	{
		if (strName == QString::fromLatin1(kOpenRequest))
			return OpenRequestFileTransferLifecycleMessageType;
		if (strName == QString::fromLatin1(kOpenAccepted))
			return OpenAcceptedFileTransferLifecycleMessageType;
		if (strName == QString::fromLatin1(kOpenRejected))
			return OpenRejectedFileTransferLifecycleMessageType;
		if (strName == QString::fromLatin1(kClose))
			return CloseFileTransferLifecycleMessageType;
		if (strName == QString::fromLatin1(kStopped))
			return StoppedFileTransferLifecycleMessageType;
		if (strName == QString::fromLatin1(kError))
			return ErrorFileTransferLifecycleMessageType;
		return InvalidFileTransferLifecycleMessageType;
	}
}

QString KFileTransferLifecycleMessageCodec::encode(
	const KFileTransferLifecycleMessage &message)
{
	QJsonObject payload;
	payload.insert(QString::fromLatin1(kGeneration), QString::number(message.nGeneration));
	if (message.type == OpenRejectedFileTransferLifecycleMessageType
		|| message.type == ErrorFileTransferLifecycleMessageType)
	{
		payload.insert(QString::fromLatin1(kErrorCode), message.strErrorCode);
	}
	return KProtocolEnvelopeCodec::encode(SessionProtocolChannel,
		typeName(message.type), message.strRequestId, 0, payload);
}

bool KFileTransferLifecycleMessageCodec::decode(const QString &strMessage,
	KFileTransferLifecycleMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("File transfer lifecycle output is null"), pErrorMessage);
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(SessionProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	if (envelope.nVersion != KProtocolConstraints::kEnvelopeSchemaVersion)
	{
		return Fail(QStringLiteral("Unsupported file transfer lifecycle version"),
			pErrorMessage);
	}
	return decode(envelope, pMessage, pErrorMessage);
}

bool KFileTransferLifecycleMessageCodec::decode(const KProtocolEnvelope &envelope,
	KFileTransferLifecycleMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("File transfer lifecycle output is null"), pErrorMessage);
	*pMessage = KFileTransferLifecycleMessage();

	KFileTransferLifecycleMessage message;
	message.type = TypeFromName(envelope.strType);
	if (message.type == InvalidFileTransferLifecycleMessageType)
		return Fail(QStringLiteral("Unknown file transfer lifecycle type"), pErrorMessage);
	const QUuid requestId(envelope.strRequestId);
	if (requestId.isNull())
		return Fail(QStringLiteral("File transfer lifecycle request id is invalid"), pErrorMessage);
	message.strRequestId = requestId.toString(QUuid::WithoutBraces);
	if (!ReadGeneration(envelope.payload, &message.nGeneration))
		return Fail(QStringLiteral("File transfer lifecycle generation is invalid"), pErrorMessage);

	if (message.type == OpenRejectedFileTransferLifecycleMessageType
		|| message.type == ErrorFileTransferLifecycleMessageType)
	{
		const QJsonValue errorCodeValue = envelope.payload.value(
			QString::fromLatin1(kErrorCode));
		if (!errorCodeValue.isString()
			|| errorCodeValue.toString().isEmpty()
			|| errorCodeValue.toString().size() > kMaximumErrorCodeCharacters)
		{
			return Fail(QStringLiteral("File transfer lifecycle error code is invalid"),
				pErrorMessage);
		}
		message.strErrorCode = errorCodeValue.toString();
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KFileTransferLifecycleMessageCodec::typeName(
	KFileTransferLifecycleMessageType type)
{
	switch (type)
	{
	case OpenRequestFileTransferLifecycleMessageType:
		return QString::fromLatin1(kOpenRequest);
	case OpenAcceptedFileTransferLifecycleMessageType:
		return QString::fromLatin1(kOpenAccepted);
	case OpenRejectedFileTransferLifecycleMessageType:
		return QString::fromLatin1(kOpenRejected);
	case CloseFileTransferLifecycleMessageType:
		return QString::fromLatin1(kClose);
	case StoppedFileTransferLifecycleMessageType:
		return QString::fromLatin1(kStopped);
	case ErrorFileTransferLifecycleMessageType:
		return QString::fromLatin1(kError);
	case InvalidFileTransferLifecycleMessageType:
	default:
		return QStringLiteral("invalid");
	}
}
