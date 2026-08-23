#include "core/protocol/filetransfercontrolmessage.h"

#include "core/protocol/protocolconstraints.h"
#include "core/protocol/protocolenvelope.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QUuid>

namespace
{
	constexpr char kListRootsRequest[] = "fileTransferListRootsRequest";
	constexpr char kListRootsResponse[] = "fileTransferListRootsResponse";
	constexpr char kListDirectoryRequest[] = "fileTransferListDirectoryRequest";
	constexpr char kListDirectoryResponse[] = "fileTransferListDirectoryResponse";
	constexpr char kCopyRequest[] = "fileTransferCopyRequest";
	constexpr char kCopyPlanBegin[] = "fileTransferCopyPlanBegin";
	constexpr char kCopyPlanDirectory[] = "fileTransferCopyPlanDirectory";
	constexpr char kCopyPlanEnd[] = "fileTransferCopyPlanEnd";
	constexpr char kFileBegin[] = "fileTransferFileBegin";
	constexpr char kAck[] = "fileTransferAck";
	constexpr char kPause[] = "fileTransferPause";
	constexpr char kResume[] = "fileTransferResume";
	constexpr char kCancel[] = "fileTransferCancel";
	constexpr char kConflict[] = "fileTransferConflict";
	constexpr char kConflictResolution[] = "fileTransferConflictResolution";
	constexpr char kFileComplete[] = "fileTransferFileComplete";
	constexpr char kTaskComplete[] = "fileTransferTaskComplete";
	constexpr char kError[] = "fileTransferError";
	constexpr char kTaskId[] = "taskId";
	constexpr char kFileId[] = "fileId";
	constexpr char kListingId[] = "listingId";
	constexpr char kSourceListingId[] = "sourceListingId";
	constexpr char kDestinationListingId[] = "destinationListingId";
	constexpr char kEntryId[] = "entryId";
	constexpr char kEntryIds[] = "entryIds";
	constexpr char kDisplayPath[] = "displayPath";
	constexpr char kPath[] = "path";
	constexpr char kCanGoUp[] = "canGoUp";
	constexpr char kNextPageToken[] = "nextPageToken";
	constexpr char kHasMore[] = "hasMore";
	constexpr char kFileName[] = "fileName";
	constexpr char kRelativePath[] = "relativePath";
	constexpr char kModifiedAtMs[] = "modifiedAtMs";
	constexpr char kSha256[] = "sha256";
	constexpr char kConflictId[] = "conflictId";
	constexpr char kApplyToRemaining[] = "applyToRemaining";
	constexpr char kDirection[] = "direction";
	constexpr char kResult[] = "result";
	constexpr char kErrorCode[] = "errorCode";
	constexpr char kOffset[] = "offset";
	constexpr char kSize[] = "size";
	constexpr char kItemCount[] = "itemCount";
	constexpr char kResolution[] = "resolution";
	constexpr char kEntries[] = "entries";
	constexpr char kName[] = "name";
	constexpr char kEntryType[] = "entryType";
	constexpr char kNavigable[] = "navigable";
	constexpr char kTransferable[] = "transferable";
	constexpr char kRoot[] = "root";
	constexpr char kDirectory[] = "directory";
	constexpr char kFile[] = "file";
	constexpr char kOverwrite[] = "overwrite";
	constexpr char kSkip[] = "skip";
	constexpr char kKeepBoth[] = "keepBoth";
	constexpr char kUpload[] = "upload";
	constexpr char kDownload[] = "download";
	constexpr char kCompleted[] = "completed";
	constexpr char kSkipped[] = "skipped";
	constexpr int kMaximumErrorCodeCharacters = 64;

	bool Fail(const QString &strError, QString *pErrorMessage)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = strError;
		return false;
	}

	bool IsValidText(const QString &strValue, int nMaximumCharacters, bool bAllowEmpty)
	{
		return (bAllowEmpty || !strValue.isEmpty())
			&& strValue.size() <= nMaximumCharacters
			&& !strValue.contains(QChar::Null);
	}

	bool IsValidFileName(const QString &strName)
	{
		return IsValidText(strName,
			KFileTransferControlMessageCodec::kMaximumEntryNameCharacters, false)
			&& strName != QStringLiteral(".")
			&& strName != QStringLiteral("..")
			&& !strName.contains(QLatin1Char('/'))
			&& !strName.contains(QLatin1Char('\\'));
	}

	bool IsValidRelativePath(const QString &strPath)
	{
		if (!IsValidText(strPath,
			KFileTransferControlMessageCodec::kMaximumRelativePathCharacters, false)
			|| strPath.startsWith(QLatin1Char('/'))
			|| strPath.startsWith(QLatin1Char('\\'))
			|| strPath.contains(QLatin1Char(':')))
		{
			return false;
		}
		QString strNormalized = strPath;
		strNormalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
		for (const QString &strPart : strNormalized.split(QLatin1Char('/'), Qt::KeepEmptyParts))
		{
			if (strPart.isEmpty()
				|| strPart == QStringLiteral(".")
				|| strPart == QStringLiteral(".."))
			{
				return false;
			}
		}
		return true;
	}

	bool NormalizeUuid(const QString &strValue, QString *pNormalized)
	{
		const QUuid uuid(strValue);
		if (uuid.isNull())
			return false;
		*pNormalized = uuid.toString(QUuid::WithoutBraces);
		return true;
	}

	bool ReadUuid(const QJsonObject &object, const char *pName, QString *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		return value.isString() && NormalizeUuid(value.toString(), pValue);
	}

	bool ReadOptionalUuid(const QJsonObject &object, const char *pName, QString *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (value.isUndefined())
		{
			pValue->clear();
			return true;
		}
		return value.isString() && NormalizeUuid(value.toString(), pValue);
	}

	bool ReadUnsigned(const QJsonObject &object, const char *pName, quint64 *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
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
		const quint64 nValue = strValue.toULongLong(&bOk);
		if (!bOk)
			return false;
		*pValue = nValue;
		return true;
	}

	bool ReadSigned(const QJsonObject &object, const char *pName, qint64 *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isString())
			return false;
		const QString strValue = value.toString();
		const int nFirstDigit = strValue.startsWith(QLatin1Char('-')) ? 1 : 0;
		if (strValue.size() <= nFirstDigit)
			return false;
		for (int nIndex = nFirstDigit; nIndex < strValue.size(); ++nIndex)
		{
			if (!strValue.at(nIndex).isDigit())
				return false;
		}
		bool bOk = false;
		const qint64 nValue = strValue.toLongLong(&bOk);
		if (!bOk)
			return false;
		*pValue = nValue;
		return true;
	}

	bool ReadRequiredBool(const QJsonObject &object, const char *pName, bool *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isBool())
			return false;
		*pValue = value.toBool();
		return true;
	}

	bool ReadToken(const QJsonObject &object, const char *pName, QString *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isString()
			|| !IsValidText(value.toString(),
				KFileTransferControlMessageCodec::kMaximumOpaqueTokenCharacters, false))
		{
			return false;
		}
		*pValue = value.toString();
		return true;
	}

	bool ReadOptionalToken(const QJsonObject &object, const char *pName, QString *pValue)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (value.isUndefined())
		{
			pValue->clear();
			return true;
		}
		return ReadToken(object, pName, pValue);
	}

	KFileTransferEntryType EntryTypeFromName(const QString &strName)
	{
		if (strName == QString::fromLatin1(kRoot))
			return RootFileTransferEntryType;
		if (strName == QString::fromLatin1(kDirectory))
			return DirectoryFileTransferEntryType;
		if (strName == QString::fromLatin1(kFile))
			return RegularFileTransferEntryType;
		return InvalidFileTransferEntryType;
	}

	KFileTransferConflictResolution ResolutionFromName(const QString &strName)
	{
		if (strName == QString::fromLatin1(kOverwrite))
			return OverwriteFileTransferConflictResolution;
		if (strName == QString::fromLatin1(kSkip))
			return SkipFileTransferConflictResolution;
		if (strName == QString::fromLatin1(kKeepBoth))
			return KeepBothFileTransferConflictResolution;
		return InvalidFileTransferConflictResolution;
	}

	KFileTransferDirection DirectionFromName(const QString &strName)
	{
		if (strName == QString::fromLatin1(kUpload))
			return UploadFileTransferDirection;
		if (strName == QString::fromLatin1(kDownload))
			return DownloadFileTransferDirection;
		return InvalidFileTransferDirection;
	}

	KFileTransferTaskResult TaskResultFromName(const QString &strName)
	{
		if (strName == QString::fromLatin1(kCompleted))
			return CompletedFileTransferTaskResult;
		if (strName == QString::fromLatin1(kSkipped))
			return SkippedFileTransferTaskResult;
		return InvalidFileTransferTaskResult;
	}

	KFileTransferControlMessageType TypeFromName(const QString &strName)
	{
		for (KFileTransferControlMessageType type : {
			ListRootsRequestFileTransferControlMessageType,
			ListRootsResponseFileTransferControlMessageType,
			ListDirectoryRequestFileTransferControlMessageType,
			ListDirectoryResponseFileTransferControlMessageType,
			CopyRequestFileTransferControlMessageType,
			CopyPlanBeginFileTransferControlMessageType,
			CopyPlanDirectoryFileTransferControlMessageType,
			CopyPlanEndFileTransferControlMessageType,
			FileBeginFileTransferControlMessageType,
			AckFileTransferControlMessageType,
			PauseFileTransferControlMessageType,
			ResumeFileTransferControlMessageType,
			CancelFileTransferControlMessageType,
			ConflictFileTransferControlMessageType,
			ConflictResolutionFileTransferControlMessageType,
			FileCompleteFileTransferControlMessageType,
			TaskCompleteFileTransferControlMessageType,
			ErrorFileTransferControlMessageType })
		{
			if (strName == KFileTransferControlMessageCodec::typeName(type))
				return type;
		}
		return InvalidFileTransferControlMessageType;
	}

	QJsonArray EncodeEntries(const QVector<KFileTransferEntry> &entryList)
	{
		QJsonArray array;
		for (const KFileTransferEntry &entry : entryList)
		{
			QJsonObject object;
			object.insert(QString::fromLatin1(kEntryId), entry.strEntryId);
			object.insert(QString::fromLatin1(kName), entry.strName);
			object.insert(QString::fromLatin1(kEntryType),
				KFileTransferControlMessageCodec::entryTypeName(entry.type));
			object.insert(QString::fromLatin1(kSize), QString::number(entry.nSize));
			object.insert(QString::fromLatin1(kModifiedAtMs),
				QString::number(entry.nModifiedAtMs));
			object.insert(QString::fromLatin1(kNavigable), entry.bNavigable);
			object.insert(QString::fromLatin1(kTransferable), entry.bTransferable);
			array.append(object);
		}
		return array;
	}

	bool ReadEntries(const QJsonObject &object, QVector<KFileTransferEntry> *pEntryList)
	{
		const QJsonValue entriesValue = object.value(QString::fromLatin1(kEntries));
		if (!entriesValue.isArray()
			|| entriesValue.toArray().size()
				> KFileTransferControlMessageCodec::kMaximumEntryCount)
		{
			return false;
		}

		QSet<QString> entryIds;
		QVector<KFileTransferEntry> entryList;
		entryList.reserve(entriesValue.toArray().size());
		for (const QJsonValue &value : entriesValue.toArray())
		{
			if (!value.isObject())
				return false;
			const QJsonObject entryObject = value.toObject();
			const QJsonValue nameValue = entryObject.value(QString::fromLatin1(kName));
			const QJsonValue typeValue = entryObject.value(QString::fromLatin1(kEntryType));
			KFileTransferEntry entry;
			if (!ReadUuid(entryObject, kEntryId, &entry.strEntryId)
				|| entryIds.contains(entry.strEntryId)
				|| !nameValue.isString() || !IsValidFileName(nameValue.toString())
				|| !typeValue.isString()
				|| !ReadUnsigned(entryObject, kSize, &entry.nSize)
				|| !ReadSigned(entryObject, kModifiedAtMs, &entry.nModifiedAtMs)
				|| !ReadRequiredBool(entryObject, kNavigable, &entry.bNavigable)
				|| !ReadRequiredBool(entryObject, kTransferable, &entry.bTransferable))
			{
				return false;
			}
			entry.type = EntryTypeFromName(typeValue.toString());
			if (entry.type == InvalidFileTransferEntryType)
				return false;
			entry.strName = nameValue.toString();
			entryIds.insert(entry.strEntryId);
			entryList.append(entry);
		}
		*pEntryList = entryList;
		return true;
	}

	QJsonArray EncodeUuidList(const QStringList &values)
	{
		QJsonArray array;
		for (const QString &strValue : values)
			array.append(strValue);
		return array;
	}

	bool ReadUuidList(const QJsonObject &object, const char *pName, QStringList *pValues)
	{
		const QJsonValue value = object.value(QString::fromLatin1(pName));
		if (!value.isArray() || value.toArray().isEmpty()
			|| value.toArray().size()
				> KFileTransferControlMessageCodec::kMaximumEntryIdCount)
		{
			return false;
		}
		QSet<QString> uniqueValues;
		QStringList values;
		for (const QJsonValue &item : value.toArray())
		{
			QString strValue;
			if (!item.isString() || !NormalizeUuid(item.toString(), &strValue)
				|| uniqueValues.contains(strValue))
			{
				return false;
			}
			uniqueValues.insert(strValue);
			values.append(strValue);
		}
		*pValues = values;
		return true;
	}

	bool ReadPagination(const QJsonObject &object,
		bool *pHasMore,
		QString *pNextPageToken)
	{
		if (!ReadRequiredBool(object, kHasMore, pHasMore)
			|| !ReadOptionalToken(object, kNextPageToken, pNextPageToken))
		{
			return false;
		}
		return *pHasMore ? !pNextPageToken->isEmpty() : pNextPageToken->isEmpty();
	}

	bool ReadListingResponse(const QJsonObject &object,
		KFileTransferControlMessage *pMessage)
	{
		const QJsonValue displayPathValue = object.value(QString::fromLatin1(kDisplayPath));
		if (!ReadUuid(object, kListingId, &pMessage->strListingId)
			|| !displayPathValue.isString()
			|| !IsValidText(displayPathValue.toString(),
				KFileTransferControlMessageCodec::kMaximumDisplayPathCharacters, true)
			|| !ReadRequiredBool(object, kCanGoUp, &pMessage->bCanGoUp)
			|| !ReadPagination(object, &pMessage->bHasMore,
				&pMessage->strNextPageToken)
			|| !ReadEntries(object, &pMessage->entryList))
		{
			return false;
		}
		pMessage->strDisplayPath = displayPathValue.toString();
		return true;
	}

	void EncodeListingResponse(const KFileTransferControlMessage &message,
		QJsonObject *pPayload)
	{
		pPayload->insert(QString::fromLatin1(kListingId), message.strListingId);
		pPayload->insert(QString::fromLatin1(kDisplayPath), message.strDisplayPath);
		pPayload->insert(QString::fromLatin1(kCanGoUp), message.bCanGoUp);
		pPayload->insert(QString::fromLatin1(kHasMore), message.bHasMore);
		if (message.bHasMore)
		{
			pPayload->insert(QString::fromLatin1(kNextPageToken),
				message.strNextPageToken);
		}
		pPayload->insert(QString::fromLatin1(kEntries), EncodeEntries(message.entryList));
	}

	bool RequiresRequestId(KFileTransferControlMessageType type)
	{
		return type == ListRootsRequestFileTransferControlMessageType
			|| type == ListRootsResponseFileTransferControlMessageType
			|| type == ListDirectoryRequestFileTransferControlMessageType
			|| type == ListDirectoryResponseFileTransferControlMessageType
			|| type == CopyRequestFileTransferControlMessageType
			|| type == CopyPlanBeginFileTransferControlMessageType
			|| type == CopyPlanDirectoryFileTransferControlMessageType
			|| type == CopyPlanEndFileTransferControlMessageType;
	}

	bool RequiresTaskId(KFileTransferControlMessageType type)
	{
		return type == CopyRequestFileTransferControlMessageType
			|| type == CopyPlanBeginFileTransferControlMessageType
			|| type == CopyPlanDirectoryFileTransferControlMessageType
			|| type == CopyPlanEndFileTransferControlMessageType
			|| type == FileBeginFileTransferControlMessageType
			|| type == AckFileTransferControlMessageType
			|| type == PauseFileTransferControlMessageType
			|| type == ResumeFileTransferControlMessageType
			|| type == CancelFileTransferControlMessageType
			|| type == ConflictFileTransferControlMessageType
			|| type == ConflictResolutionFileTransferControlMessageType
			|| type == FileCompleteFileTransferControlMessageType
			|| type == TaskCompleteFileTransferControlMessageType;
	}

	bool RequiresFileId(KFileTransferControlMessageType type)
	{
		return type == FileBeginFileTransferControlMessageType
			|| type == AckFileTransferControlMessageType
			|| type == ConflictFileTransferControlMessageType
			|| type == ConflictResolutionFileTransferControlMessageType
			|| type == FileCompleteFileTransferControlMessageType;
	}
}

QString KFileTransferControlMessageCodec::encode(
	const KFileTransferControlMessage &message)
{
	QJsonObject payload;
	if (!message.strTaskId.isEmpty())
		payload.insert(QString::fromLatin1(kTaskId), message.strTaskId);
	if (!message.strFileId.isEmpty())
		payload.insert(QString::fromLatin1(kFileId), message.strFileId);

	if (message.type == ListRootsRequestFileTransferControlMessageType)
	{
		if (!message.strNextPageToken.isEmpty())
		{
			payload.insert(QString::fromLatin1(kNextPageToken),
				message.strNextPageToken);
		}
	}
	else if (message.type == ListRootsResponseFileTransferControlMessageType
		|| message.type == ListDirectoryResponseFileTransferControlMessageType)
	{
		EncodeListingResponse(message, &payload);
	}
	else if (message.type == ListDirectoryRequestFileTransferControlMessageType)
	{
		if (!message.strDisplayPath.isEmpty())
			payload.insert(QString::fromLatin1(kDisplayPath), message.strDisplayPath);
		if (!message.strListingId.isEmpty())
			payload.insert(QString::fromLatin1(kListingId), message.strListingId);
		if (!message.strEntryId.isEmpty())
			payload.insert(QString::fromLatin1(kEntryId), message.strEntryId);
		if (!message.strNextPageToken.isEmpty())
		{
			payload.insert(QString::fromLatin1(kNextPageToken),
				message.strNextPageToken);
		}
	}
	else if (message.type == CopyRequestFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kSourceListingId), message.strSourceListingId);
		payload.insert(QString::fromLatin1(kDestinationListingId),
			message.strDestinationListingId);
		payload.insert(QString::fromLatin1(kEntryIds), EncodeUuidList(message.entryIdList));
		payload.insert(QString::fromLatin1(kDirection), directionName(message.direction));
	}
	else if (message.type == CopyPlanBeginFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kDestinationListingId),
			message.strDestinationListingId);
		payload.insert(QString::fromLatin1(kDirection), directionName(message.direction));
		payload.insert(QString::fromLatin1(kItemCount),
			QString::number(message.nItemCount));
		payload.insert(QString::fromLatin1(kSize), QString::number(message.nSize));
	}
	else if (message.type == CopyPlanDirectoryFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kRelativePath), message.strRelativePath);
		payload.insert(QString::fromLatin1(kModifiedAtMs),
			QString::number(message.nModifiedAtMs));
	}
	else if (message.type == CopyPlanEndFileTransferControlMessageType)
	{
		if (message.taskResult != InvalidFileTransferTaskResult)
		{
			payload.insert(QString::fromLatin1(kResult),
				taskResultName(message.taskResult));
		}
	}
	else if (message.type == FileBeginFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kFileName), message.strFileName);
		payload.insert(QString::fromLatin1(kRelativePath), message.strRelativePath);
		payload.insert(QString::fromLatin1(kDestinationListingId),
			message.strDestinationListingId);
		payload.insert(QString::fromLatin1(kSize), QString::number(message.nSize));
		payload.insert(QString::fromLatin1(kModifiedAtMs),
			QString::number(message.nModifiedAtMs));
	}
	else if (message.type == AckFileTransferControlMessageType)
		payload.insert(QString::fromLatin1(kOffset), QString::number(message.nOffset));
	else if (message.type == ConflictFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kConflictId), message.strConflictId);
		payload.insert(QString::fromLatin1(kFileName), message.strFileName);
	}
	else if (message.type == ConflictResolutionFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kConflictId), message.strConflictId);
		payload.insert(QString::fromLatin1(kResolution),
			conflictResolutionName(message.conflictResolution));
		payload.insert(QString::fromLatin1(kApplyToRemaining), message.bApplyToRemaining);
	}
	else if (message.type == FileCompleteFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kSize), QString::number(message.nSize));
		payload.insert(QString::fromLatin1(kSha256),
			QString::fromLatin1(message.sha256.toHex()));
	}
	else if (message.type == TaskCompleteFileTransferControlMessageType)
	{
		payload.insert(QString::fromLatin1(kResult), taskResultName(message.taskResult));
	}
	else if (message.type == ErrorFileTransferControlMessageType)
		payload.insert(QString::fromLatin1(kErrorCode), message.strErrorCode);

	return KProtocolEnvelopeCodec::encode(FileControlProtocolChannel,
		typeName(message.type), message.strRequestId, 0, payload);
}

bool KFileTransferControlMessageCodec::decode(const QString &strMessage,
	KFileTransferControlMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("File transfer control output is null"), pErrorMessage);
	KProtocolEnvelope envelope;
	if (!KProtocolEnvelopeCodec::decode(FileControlProtocolChannel,
		strMessage, &envelope, pErrorMessage))
	{
		return false;
	}
	if (envelope.nVersion != KProtocolConstraints::kEnvelopeSchemaVersion)
		return Fail(QStringLiteral("Unsupported file transfer control version"), pErrorMessage);
	return decode(envelope, pMessage, pErrorMessage);
}

bool KFileTransferControlMessageCodec::decode(const KProtocolEnvelope &envelope,
	KFileTransferControlMessage *pMessage,
	QString *pErrorMessage)
{
	if (pMessage == nullptr)
		return Fail(QStringLiteral("File transfer control output is null"), pErrorMessage);
	*pMessage = KFileTransferControlMessage();

	KFileTransferControlMessage message;
	message.type = TypeFromName(envelope.strType);
	if (message.type == InvalidFileTransferControlMessageType)
		return Fail(QStringLiteral("Unknown file transfer control type"), pErrorMessage);
	if (!envelope.strRequestId.isEmpty()
		&& !NormalizeUuid(envelope.strRequestId, &message.strRequestId))
	{
		return Fail(QStringLiteral("File transfer request id is invalid"), pErrorMessage);
	}
	if (!ReadOptionalUuid(envelope.payload, kTaskId, &message.strTaskId)
		|| !ReadOptionalUuid(envelope.payload, kFileId, &message.strFileId))
	{
		return Fail(QStringLiteral("File transfer entity id is invalid"), pErrorMessage);
	}
	if (!message.strFileId.isEmpty() && message.strTaskId.isEmpty())
		return Fail(QStringLiteral("File transfer file id requires a task id"), pErrorMessage);
	if (RequiresRequestId(message.type) && message.strRequestId.isEmpty())
		return Fail(QStringLiteral("File transfer request id is required"), pErrorMessage);
	if (RequiresTaskId(message.type) && message.strTaskId.isEmpty())
		return Fail(QStringLiteral("File transfer task id is required"), pErrorMessage);
	if (RequiresFileId(message.type) && message.strFileId.isEmpty())
		return Fail(QStringLiteral("File transfer file id is required"), pErrorMessage);
	if (message.type == ErrorFileTransferControlMessageType
		&& message.strRequestId.isEmpty() && message.strTaskId.isEmpty())
	{
		return Fail(QStringLiteral("File transfer error correlation id is required"),
			pErrorMessage);
	}

	const QJsonObject &payload = envelope.payload;
	if (message.type == ListRootsRequestFileTransferControlMessageType)
	{
		if (!ReadOptionalToken(payload, kNextPageToken, &message.strNextPageToken))
			return Fail(QStringLiteral("File transfer page token is invalid"), pErrorMessage);
	}
	else if (message.type == ListRootsResponseFileTransferControlMessageType)
	{
		if (!ReadListingResponse(payload, &message))
			return Fail(QStringLiteral("File transfer roots are invalid"), pErrorMessage);
		for (const KFileTransferEntry &entry : message.entryList)
		{
			if (entry.type != RootFileTransferEntryType)
				return Fail(QStringLiteral("File transfer root entry type is invalid"),
					pErrorMessage);
		}
	}
	else if (message.type == ListDirectoryRequestFileTransferControlMessageType)
	{
		const QJsonValue displayPathValue = payload.value(QString::fromLatin1(kDisplayPath));
		const QJsonValue pathValue = payload.value(QString::fromLatin1(kPath));
		if (!displayPathValue.isUndefined() && !pathValue.isUndefined())
			return Fail(QStringLiteral("File transfer directory selector is ambiguous"),
				pErrorMessage);
		const QJsonValue selectedPathValue = displayPathValue.isUndefined()
			? pathValue : displayPathValue;
		const bool bHasDisplayPath = !selectedPathValue.isUndefined();
		if (bHasDisplayPath
			&& (!selectedPathValue.isString()
				|| !IsValidText(selectedPathValue.toString(),
					KFileTransferControlMessageCodec::kMaximumDisplayPathCharacters, false)))
		{
			return Fail(QStringLiteral("File transfer display path is invalid"), pErrorMessage);
		}
		if (!ReadOptionalUuid(payload, kListingId, &message.strListingId)
			|| !ReadOptionalUuid(payload, kEntryId, &message.strEntryId)
			|| !ReadOptionalToken(payload, kNextPageToken, &message.strNextPageToken))
		{
			return Fail(QStringLiteral("File transfer directory selector is invalid"),
				pErrorMessage);
		}
		const bool bHasOpaqueTarget = !message.strListingId.isEmpty()
			|| !message.strEntryId.isEmpty();
		if (bHasDisplayPath == bHasOpaqueTarget
			|| (bHasOpaqueTarget
				&& (message.strListingId.isEmpty() || message.strEntryId.isEmpty())))
		{
			return Fail(QStringLiteral("File transfer directory selector is invalid"),
				pErrorMessage);
		}
		if (bHasDisplayPath)
			message.strDisplayPath = selectedPathValue.toString();
	}
	else if (message.type == ListDirectoryResponseFileTransferControlMessageType)
	{
		if (!ReadListingResponse(payload, &message))
		{
			return Fail(QStringLiteral("File transfer directory listing is invalid"),
				pErrorMessage);
		}
		for (const KFileTransferEntry &entry : message.entryList)
		{
			if (entry.type == RootFileTransferEntryType)
				return Fail(QStringLiteral("File transfer directory entry type is invalid"),
					pErrorMessage);
		}
	}
	else if (message.type == CopyRequestFileTransferControlMessageType)
	{
		const QJsonValue directionValue = payload.value(QString::fromLatin1(kDirection));
		if (!ReadUuid(payload, kSourceListingId, &message.strSourceListingId)
			|| !ReadUuid(payload, kDestinationListingId,
				&message.strDestinationListingId)
			|| !ReadUuidList(payload, kEntryIds, &message.entryIdList)
			|| !directionValue.isString())
		{
			return Fail(QStringLiteral("File transfer copy request is invalid"), pErrorMessage);
		}
		message.direction = DirectionFromName(directionValue.toString());
		if (message.direction == InvalidFileTransferDirection)
			return Fail(QStringLiteral("File transfer direction is invalid"), pErrorMessage);
	}
	else if (message.type == CopyPlanBeginFileTransferControlMessageType)
	{
		const QJsonValue directionValue = payload.value(QString::fromLatin1(kDirection));
		if (!ReadUuid(payload, kDestinationListingId,
				&message.strDestinationListingId)
			|| !directionValue.isString()
			|| !ReadUnsigned(payload, kItemCount, &message.nItemCount)
			|| message.nItemCount == 0
			|| message.nItemCount
				> KFileTransferControlMessageCodec::kMaximumCopyPlanItemCount
			|| !ReadUnsigned(payload, kSize, &message.nSize))
		{
			return Fail(QStringLiteral("File transfer copy plan is invalid"), pErrorMessage);
		}
		message.direction = DirectionFromName(directionValue.toString());
		if (message.direction == InvalidFileTransferDirection)
			return Fail(QStringLiteral("File transfer direction is invalid"), pErrorMessage);
	}
	else if (message.type == CopyPlanDirectoryFileTransferControlMessageType)
	{
		const QJsonValue relativePathValue = payload.value(
			QString::fromLatin1(kRelativePath));
		if (!relativePathValue.isString()
			|| !IsValidRelativePath(relativePathValue.toString())
			|| !ReadSigned(payload, kModifiedAtMs, &message.nModifiedAtMs))
		{
			return Fail(QStringLiteral("File transfer plan directory is invalid"),
				pErrorMessage);
		}
		message.strRelativePath = relativePathValue.toString();
	}
	else if (message.type == CopyPlanEndFileTransferControlMessageType)
	{
		const QJsonValue resultValue = payload.value(QString::fromLatin1(kResult));
		if (!resultValue.isUndefined())
		{
			if (!resultValue.isString())
				return Fail(QStringLiteral("File transfer copy plan result is invalid"),
					pErrorMessage);
			message.taskResult = TaskResultFromName(resultValue.toString());
			if (message.taskResult == InvalidFileTransferTaskResult)
				return Fail(QStringLiteral("File transfer copy plan result is invalid"),
					pErrorMessage);
		}
	}
	else if (message.type == FileBeginFileTransferControlMessageType)
	{
		const QJsonValue fileNameValue = payload.value(QString::fromLatin1(kFileName));
		const QJsonValue relativePathValue = payload.value(QString::fromLatin1(kRelativePath));
		if (!fileNameValue.isString() || !IsValidFileName(fileNameValue.toString())
			|| !relativePathValue.isString()
			|| !IsValidRelativePath(relativePathValue.toString())
			|| !ReadUuid(payload, kDestinationListingId,
				&message.strDestinationListingId)
			|| !ReadUnsigned(payload, kSize, &message.nSize)
			|| !ReadSigned(payload, kModifiedAtMs, &message.nModifiedAtMs))
		{
			return Fail(QStringLiteral("File transfer file metadata is invalid"), pErrorMessage);
		}
		message.strFileName = fileNameValue.toString();
		message.strRelativePath = relativePathValue.toString();
	}
	else if (message.type == AckFileTransferControlMessageType)
	{
		if (!ReadUnsigned(payload, kOffset, &message.nOffset))
			return Fail(QStringLiteral("File transfer acknowledgement is invalid"), pErrorMessage);
	}
	else if (message.type == ConflictFileTransferControlMessageType)
	{
		const QJsonValue fileNameValue = payload.value(QString::fromLatin1(kFileName));
		if (!ReadUuid(payload, kConflictId, &message.strConflictId)
			|| !fileNameValue.isString() || !IsValidFileName(fileNameValue.toString()))
		{
			return Fail(QStringLiteral("File transfer conflict is invalid"), pErrorMessage);
		}
		message.strFileName = fileNameValue.toString();
	}
	else if (message.type == ConflictResolutionFileTransferControlMessageType)
	{
		const QJsonValue resolutionValue = payload.value(QString::fromLatin1(kResolution));
		if (!ReadUuid(payload, kConflictId, &message.strConflictId)
			|| !resolutionValue.isString()
			|| !ReadRequiredBool(payload, kApplyToRemaining,
				&message.bApplyToRemaining))
		{
			return Fail(QStringLiteral("File transfer conflict resolution is invalid"),
				pErrorMessage);
		}
		message.conflictResolution = ResolutionFromName(resolutionValue.toString());
		if (message.conflictResolution == InvalidFileTransferConflictResolution)
			return Fail(QStringLiteral("File transfer conflict resolution is invalid"),
				pErrorMessage);
	}
	else if (message.type == FileCompleteFileTransferControlMessageType)
	{
		const QJsonValue sha256Value = payload.value(QString::fromLatin1(kSha256));
		if (!ReadUnsigned(payload, kSize, &message.nSize)
			|| !sha256Value.isString()
			|| sha256Value.toString().size() != kSha256Bytes * 2)
		{
			return Fail(QStringLiteral("File transfer completion metadata is invalid"),
				pErrorMessage);
		}
		const QByteArray hex = sha256Value.toString().toLatin1();
		for (const char character : hex)
		{
			if (!((character >= '0' && character <= '9')
				|| (character >= 'a' && character <= 'f')
				|| (character >= 'A' && character <= 'F')))
			{
				return Fail(QStringLiteral("File transfer SHA-256 is invalid"), pErrorMessage);
			}
		}
		message.sha256 = QByteArray::fromHex(hex);
		if (message.sha256.size() != kSha256Bytes)
			return Fail(QStringLiteral("File transfer SHA-256 is invalid"), pErrorMessage);
	}
	else if (message.type == TaskCompleteFileTransferControlMessageType)
	{
		const QJsonValue resultValue = payload.value(QString::fromLatin1(kResult));
		if (!resultValue.isString())
			return Fail(QStringLiteral("File transfer task result is invalid"), pErrorMessage);
		message.taskResult = TaskResultFromName(resultValue.toString());
		if (message.taskResult == InvalidFileTransferTaskResult)
			return Fail(QStringLiteral("File transfer task result is invalid"), pErrorMessage);
	}
	else if (message.type == ErrorFileTransferControlMessageType)
	{
		const QJsonValue errorCodeValue = payload.value(QString::fromLatin1(kErrorCode));
		if (!errorCodeValue.isString()
			|| !IsValidText(errorCodeValue.toString(), kMaximumErrorCodeCharacters, false))
		{
			return Fail(QStringLiteral("File transfer error code is invalid"), pErrorMessage);
		}
		message.strErrorCode = errorCodeValue.toString();
	}

	*pMessage = message;
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

QString KFileTransferControlMessageCodec::typeName(KFileTransferControlMessageType type)
{
	switch (type)
	{
	case ListRootsRequestFileTransferControlMessageType:
		return QString::fromLatin1(kListRootsRequest);
	case ListRootsResponseFileTransferControlMessageType:
		return QString::fromLatin1(kListRootsResponse);
	case ListDirectoryRequestFileTransferControlMessageType:
		return QString::fromLatin1(kListDirectoryRequest);
	case ListDirectoryResponseFileTransferControlMessageType:
		return QString::fromLatin1(kListDirectoryResponse);
	case CopyRequestFileTransferControlMessageType:
		return QString::fromLatin1(kCopyRequest);
	case CopyPlanBeginFileTransferControlMessageType:
		return QString::fromLatin1(kCopyPlanBegin);
	case CopyPlanDirectoryFileTransferControlMessageType:
		return QString::fromLatin1(kCopyPlanDirectory);
	case CopyPlanEndFileTransferControlMessageType:
		return QString::fromLatin1(kCopyPlanEnd);
	case FileBeginFileTransferControlMessageType:
		return QString::fromLatin1(kFileBegin);
	case AckFileTransferControlMessageType:
		return QString::fromLatin1(kAck);
	case PauseFileTransferControlMessageType:
		return QString::fromLatin1(kPause);
	case ResumeFileTransferControlMessageType:
		return QString::fromLatin1(kResume);
	case CancelFileTransferControlMessageType:
		return QString::fromLatin1(kCancel);
	case ConflictFileTransferControlMessageType:
		return QString::fromLatin1(kConflict);
	case ConflictResolutionFileTransferControlMessageType:
		return QString::fromLatin1(kConflictResolution);
	case FileCompleteFileTransferControlMessageType:
		return QString::fromLatin1(kFileComplete);
	case TaskCompleteFileTransferControlMessageType:
		return QString::fromLatin1(kTaskComplete);
	case ErrorFileTransferControlMessageType:
		return QString::fromLatin1(kError);
	case InvalidFileTransferControlMessageType:
	default:
		return QStringLiteral("invalid");
	}
}

QString KFileTransferControlMessageCodec::entryTypeName(KFileTransferEntryType type)
{
	if (type == RootFileTransferEntryType)
		return QString::fromLatin1(kRoot);
	if (type == DirectoryFileTransferEntryType)
		return QString::fromLatin1(kDirectory);
	if (type == RegularFileTransferEntryType)
		return QString::fromLatin1(kFile);
	return QStringLiteral("invalid");
}

QString KFileTransferControlMessageCodec::conflictResolutionName(
	KFileTransferConflictResolution resolution)
{
	if (resolution == OverwriteFileTransferConflictResolution)
		return QString::fromLatin1(kOverwrite);
	if (resolution == SkipFileTransferConflictResolution)
		return QString::fromLatin1(kSkip);
	if (resolution == KeepBothFileTransferConflictResolution)
		return QString::fromLatin1(kKeepBoth);
	return QStringLiteral("invalid");
}

QString KFileTransferControlMessageCodec::directionName(KFileTransferDirection direction)
{
	if (direction == UploadFileTransferDirection)
		return QString::fromLatin1(kUpload);
	if (direction == DownloadFileTransferDirection)
		return QString::fromLatin1(kDownload);
	return QStringLiteral("invalid");
}

QString KFileTransferControlMessageCodec::taskResultName(KFileTransferTaskResult result)
{
	if (result == CompletedFileTransferTaskResult)
		return QString::fromLatin1(kCompleted);
	if (result == SkippedFileTransferTaskResult)
		return QString::fromLatin1(kSkipped);
	return QStringLiteral("invalid");
}
