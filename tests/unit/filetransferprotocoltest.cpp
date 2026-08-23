#include "core/protocol/filetransfercontrolmessage.h"
#include "core/protocol/filetransferlifecyclemessage.h"
#include "core/protocol/protocolenvelope.h"
#include "core/protocol/sessionmessage.h"
#include "core/security/permissionscope.h"
#include "core/session/capabilitynegotiator.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>

#include <limits>

namespace
{
	constexpr char kRequestId[] = "550e8400-e29b-41d4-a716-446655440000";
	constexpr char kTaskId[] = "6ba7b810-9dad-41d1-80b4-00c04fd430c8";
	constexpr char kFileId[] = "7ca76045-2ded-4f65-9912-16ff5ee3d0cc";
	constexpr char kListingId[] = "123e4567-e89b-12d3-a456-426614174000";
	constexpr char kSourceListingId[] = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
	constexpr char kDestinationListingId[] = "9c858901-8a57-4791-81fe-4c455b099bc9";
	constexpr char kEntryId[] = "16fd2706-8baf-433b-82eb-8c7fada847da";
	constexpr char kSecondEntryId[] = "e902893a-9d22-3c7e-a7b8-d6e313b71d9f";
	constexpr char kConflictId[] = "1c6c7f2e-8507-4f15-9c6c-df184450e13b";
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void TestPermissionScopeNames()
	{
		const KPermissionScopes allPermissions = KPermissionScopes::fromInt(
			kAllPermissionScopeBits);
		const QStringList expectedNames = {
			QStringLiteral("viewScreen"),
			QStringLiteral("inputControl"),
			QStringLiteral("clipboard"),
			QStringLiteral("terminal"),
			QStringLiteral("fileTransfer")
		};
		Check(PermissionScopeNames(allPermissions) == expectedNames,
			QStringLiteral("permission scope names are stable and ordered"));

		KPermissionScopes parsed;
		Check(PermissionScopesFromNames(expectedNames, &parsed)
			&& parsed == allPermissions,
			QStringLiteral("permission scope names round-trip"));
		parsed = KPermissionScopes(ViewScreenPermissionScope);
		Check(!PermissionScopesFromNames({ QStringLiteral("filetransfer") }, &parsed)
			&& parsed == KPermissionScopes(ViewScreenPermissionScope),
			QStringLiteral("permission parsing rejects unstable aliases without partial output"));
	}

	KSessionCapabilities CompatibleCapabilities()
	{
		KSessionCapabilities capabilities;
		capabilities.supportedCodecs = { QStringLiteral("h264") };
		capabilities.supportedChannels = {
			QStringLiteral("video"),
			QStringLiteral("session"),
			QStringLiteral("input"),
			QStringLiteral("file-control"),
			QStringLiteral("file-data")
		};
		return capabilities;
	}

	void TestFileTransferCapabilities()
	{
		const KSessionCapabilities local = CompatibleCapabilities();
		KSessionCapabilities remote = CompatibleCapabilities();
		KCapabilityNegotiationResult result = KCapabilityNegotiator::negotiate(local, remote);
		Check(result.succeeded() && result.capabilities.bFileTransfer,
			QStringLiteral("both file channels derive file transfer availability"));

		remote.supportedChannels.removeAll(QStringLiteral("file-data"));
		result = KCapabilityNegotiator::negotiate(local, remote);
		Check(result.succeeded() && !result.capabilities.bFileTransfer
			&& result.capabilities.channels.contains(QStringLiteral("file-control")),
			QStringLiteral("partial file channel support does not advertise availability"));
	}

	void TestLifecycleMessages()
	{
		for (KFileTransferLifecycleMessageType type : {
			OpenRequestFileTransferLifecycleMessageType,
			OpenAcceptedFileTransferLifecycleMessageType,
			OpenRejectedFileTransferLifecycleMessageType,
			CloseFileTransferLifecycleMessageType,
			StoppedFileTransferLifecycleMessageType,
			ErrorFileTransferLifecycleMessageType })
		{
			KFileTransferLifecycleMessage source;
			source.type = type;
			source.strRequestId = QString::fromLatin1(kRequestId);
			source.nGeneration = std::numeric_limits<quint64>::max();
			if (type == OpenRejectedFileTransferLifecycleMessageType
				|| type == ErrorFileTransferLifecycleMessageType)
			{
				source.strErrorCode = QStringLiteral("permission_denied");
			}

			KFileTransferLifecycleMessage decoded;
			Check(KFileTransferLifecycleMessageCodec::decode(
					KFileTransferLifecycleMessageCodec::encode(source), &decoded, nullptr)
				&& decoded.type == source.type
				&& decoded.strRequestId == source.strRequestId
				&& decoded.nGeneration == source.nGeneration
				&& decoded.strErrorCode == source.strErrorCode,
				QStringLiteral("file transfer lifecycle type round-trips: %1")
					.arg(KFileTransferLifecycleMessageCodec::typeName(type)));
		}

		KFileTransferLifecycleMessage decoded;
		Check(!KFileTransferLifecycleMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"fileTransferOpenRequest\","
				"\"requestId\":\"550e8400-e29b-41d4-a716-446655440000\","
				"\"generation\":1}"), &decoded, nullptr),
			QStringLiteral("numeric lifecycle generation is rejected"));
		Check(!KFileTransferLifecycleMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"fileTransferError\","
				"\"requestId\":\"bad\",\"generation\":\"1\",\"errorCode\":\"io\"}"),
			&decoded, nullptr), QStringLiteral("invalid lifecycle UUID is rejected"));
	}

	KFileTransferControlMessage MessageForType(KFileTransferControlMessageType type)
	{
		KFileTransferControlMessage message;
		message.type = type;
		if (type == ListRootsRequestFileTransferControlMessageType
			|| type == ListRootsResponseFileTransferControlMessageType
			|| type == ListDirectoryRequestFileTransferControlMessageType
			|| type == ListDirectoryResponseFileTransferControlMessageType
			|| type == CopyRequestFileTransferControlMessageType
			|| type == CopyPlanBeginFileTransferControlMessageType
			|| type == CopyPlanDirectoryFileTransferControlMessageType
			|| type == CopyPlanEndFileTransferControlMessageType
			|| type == ErrorFileTransferControlMessageType)
		{
			message.strRequestId = QString::fromLatin1(kRequestId);
		}
		if (type == CopyRequestFileTransferControlMessageType
			|| type == CopyPlanBeginFileTransferControlMessageType
			|| type == CopyPlanDirectoryFileTransferControlMessageType
			|| type == CopyPlanEndFileTransferControlMessageType
			|| (type >= FileBeginFileTransferControlMessageType
				&& type <= TaskCompleteFileTransferControlMessageType))
		{
			message.strTaskId = QString::fromLatin1(kTaskId);
		}
		if (type == FileBeginFileTransferControlMessageType
			|| type == AckFileTransferControlMessageType
			|| type == ConflictFileTransferControlMessageType
			|| type == ConflictResolutionFileTransferControlMessageType
			|| type == FileCompleteFileTransferControlMessageType)
		{
			message.strFileId = QString::fromLatin1(kFileId);
		}

		if (type == ListRootsRequestFileTransferControlMessageType)
			message.strNextPageToken = QStringLiteral("roots-page-2");
		if (type == ListRootsResponseFileTransferControlMessageType
			|| type == ListDirectoryResponseFileTransferControlMessageType)
		{
			message.strListingId = QString::fromLatin1(kListingId);
			message.strDisplayPath = type == ListRootsResponseFileTransferControlMessageType
				? QStringLiteral("This PC") : QStringLiteral("共享/资料");
			message.bCanGoUp = type == ListDirectoryResponseFileTransferControlMessageType;
			message.bHasMore = true;
			message.strNextPageToken = QStringLiteral("listing-page-2");
			KFileTransferEntry entry;
			entry.type = type == ListRootsResponseFileTransferControlMessageType
				? RootFileTransferEntryType : RegularFileTransferEntryType;
			entry.strEntryId = QString::fromLatin1(kEntryId);
			entry.strName = type == ListRootsResponseFileTransferControlMessageType
				? QStringLiteral("C:") : QStringLiteral("报告.txt");
			entry.nSize = std::numeric_limits<quint64>::max();
			entry.nModifiedAtMs = -123456789;
			entry.bNavigable = entry.type != RegularFileTransferEntryType;
			entry.bTransferable = entry.type == RegularFileTransferEntryType;
			message.entryList.append(entry);
		}
		if (type == ListDirectoryRequestFileTransferControlMessageType)
		{
			message.strListingId = QString::fromLatin1(kListingId);
			message.strEntryId = QString::fromLatin1(kEntryId);
			message.strNextPageToken = QStringLiteral("directory-page-2");
		}
		if (type == CopyRequestFileTransferControlMessageType)
		{
			message.strSourceListingId = QString::fromLatin1(kSourceListingId);
			message.strDestinationListingId = QString::fromLatin1(kDestinationListingId);
			message.entryIdList = {
				QString::fromLatin1(kEntryId), QString::fromLatin1(kSecondEntryId)
			};
			message.direction = DownloadFileTransferDirection;
		}
		if (type == CopyPlanBeginFileTransferControlMessageType)
		{
			message.strDestinationListingId = QString::fromLatin1(kDestinationListingId);
			message.direction = UploadFileTransferDirection;
			message.nItemCount = 50000;
			message.nSize = std::numeric_limits<quint64>::max();
		}
		if (type == CopyPlanDirectoryFileTransferControlMessageType)
		{
			message.strRelativePath = QStringLiteral("资料/空目录");
			message.nModifiedAtMs = std::numeric_limits<qint64>::min();
		}
		if (type == FileBeginFileTransferControlMessageType)
		{
			message.strFileName = QStringLiteral("报告.txt");
			message.strRelativePath = QStringLiteral("资料/报告.txt");
			message.strDestinationListingId = QString::fromLatin1(kDestinationListingId);
			message.nSize = std::numeric_limits<quint64>::max();
			message.nModifiedAtMs = std::numeric_limits<qint64>::min();
		}
		if (type == AckFileTransferControlMessageType)
			message.nOffset = std::numeric_limits<quint64>::max();
		if (type == ConflictFileTransferControlMessageType)
		{
			message.strConflictId = QString::fromLatin1(kConflictId);
			message.strFileName = QStringLiteral("报告.txt");
		}
		if (type == ConflictResolutionFileTransferControlMessageType)
		{
			message.strConflictId = QString::fromLatin1(kConflictId);
			message.conflictResolution = KeepBothFileTransferConflictResolution;
			message.bApplyToRemaining = true;
		}
		if (type == FileCompleteFileTransferControlMessageType)
		{
			message.nSize = std::numeric_limits<quint64>::max();
			message.sha256 = QByteArray::fromHex(
				"00112233445566778899aabbccddeeff"
				"00112233445566778899aabbccddeeff");
		}
		if (type == TaskCompleteFileTransferControlMessageType)
			message.taskResult = SkippedFileTransferTaskResult;
		if (type == ErrorFileTransferControlMessageType)
			message.strErrorCode = QStringLiteral("write_failed");
		return message;
	}

	bool EntriesMatch(const QVector<KFileTransferEntry> &left,
		const QVector<KFileTransferEntry> &right)
	{
		if (left.size() != right.size())
			return false;
		for (int nIndex = 0; nIndex < left.size(); ++nIndex)
		{
			const KFileTransferEntry &leftEntry = left.at(nIndex);
			const KFileTransferEntry &rightEntry = right.at(nIndex);
			if (leftEntry.type != rightEntry.type
				|| leftEntry.strEntryId != rightEntry.strEntryId
				|| leftEntry.strName != rightEntry.strName
				|| leftEntry.nSize != rightEntry.nSize
				|| leftEntry.nModifiedAtMs != rightEntry.nModifiedAtMs
				|| leftEntry.bNavigable != rightEntry.bNavigable
				|| leftEntry.bTransferable != rightEntry.bTransferable)
			{
				return false;
			}
		}
		return true;
	}

	void TestControlMessages()
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
			const KFileTransferControlMessage source = MessageForType(type);
			KFileTransferControlMessage decoded;
			Check(KFileTransferControlMessageCodec::decode(
					KFileTransferControlMessageCodec::encode(source), &decoded, nullptr)
				&& decoded.type == source.type
				&& decoded.strRequestId == source.strRequestId
				&& decoded.strTaskId == source.strTaskId
				&& decoded.strFileId == source.strFileId
				&& decoded.strListingId == source.strListingId
				&& decoded.strSourceListingId == source.strSourceListingId
				&& decoded.strDestinationListingId == source.strDestinationListingId
				&& decoded.strEntryId == source.strEntryId
				&& decoded.entryIdList == source.entryIdList
				&& decoded.strDisplayPath == source.strDisplayPath
				&& decoded.bCanGoUp == source.bCanGoUp
				&& decoded.strNextPageToken == source.strNextPageToken
				&& decoded.bHasMore == source.bHasMore
				&& decoded.strFileName == source.strFileName
				&& decoded.strRelativePath == source.strRelativePath
				&& decoded.nModifiedAtMs == source.nModifiedAtMs
				&& decoded.sha256 == source.sha256
				&& decoded.strConflictId == source.strConflictId
				&& decoded.bApplyToRemaining == source.bApplyToRemaining
				&& decoded.strErrorCode == source.strErrorCode
				&& decoded.nOffset == source.nOffset
				&& decoded.nSize == source.nSize
				&& decoded.nItemCount == source.nItemCount
				&& decoded.conflictResolution == source.conflictResolution
				&& decoded.direction == source.direction
				&& decoded.taskResult == source.taskResult
				&& EntriesMatch(decoded.entryList, source.entryList),
				QStringLiteral("file transfer control type round-trips: %1")
					.arg(KFileTransferControlMessageCodec::typeName(type)));
		}

		KFileTransferControlMessage pathRequest = MessageForType(
			ListDirectoryRequestFileTransferControlMessageType);
		pathRequest.strListingId.clear();
		pathRequest.strEntryId.clear();
		pathRequest.strDisplayPath = QStringLiteral("共享/资料");
		KFileTransferControlMessage decoded;
		Check(KFileTransferControlMessageCodec::decode(
				KFileTransferControlMessageCodec::encode(pathRequest), &decoded, nullptr)
			&& decoded.strDisplayPath == pathRequest.strDisplayPath,
			QStringLiteral("display-path directory selector round-trips"));

		const KFileTransferControlMessage fileBegin = MessageForType(
			FileBeginFileTransferControlMessageType);
		const QJsonObject encodedObject = QJsonDocument::fromJson(
			KFileTransferControlMessageCodec::encode(fileBegin).toUtf8()).object();
		Check(encodedObject.value(QStringLiteral("size")).isString()
			&& encodedObject.value(QStringLiteral("modifiedAtMs")).isString(),
			QStringLiteral("file metadata integers are encoded as JSON strings"));
		const QString strCopyMessage = KFileTransferControlMessageCodec::encode(
			MessageForType(CopyRequestFileTransferControlMessageType));
		Check(!strCopyMessage.contains(QStringLiteral("sourcePath"))
			&& !strCopyMessage.contains(QStringLiteral("destinationPath")),
			QStringLiteral("copy requests do not expose real paths"));

		Check(!KFileTransferControlMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"fileTransferFileBegin\","
				"\"taskId\":\"6ba7b810-9dad-41d1-80b4-00c04fd430c8\","
				"\"fileId\":\"7ca76045-2ded-4f65-9912-16ff5ee3d0cc\","
				"\"destinationListingId\":\"9c858901-8a57-4791-81fe-4c455b099bc9\","
				"\"fileName\":\"x\",\"relativePath\":\"x\","
				"\"size\":1,\"modifiedAtMs\":\"0\"}"), &decoded, nullptr),
			QStringLiteral("numeric file size is rejected"));
		Check(!KFileTransferControlMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"fileTransferAck\","
				"\"taskId\":\"6ba7b810-9dad-41d1-80b4-00c04fd430c8\","
				"\"fileId\":\"7ca76045-2ded-4f65-9912-16ff5ee3d0cc\","
				"\"offset\":\"18446744073709551616\"}"), &decoded, nullptr),
			QStringLiteral("overflowing file offset is rejected"));
		Check(!KFileTransferControlMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"fileTransferUnknown\","
				"\"requestId\":\"550e8400-e29b-41d4-a716-446655440000\"}"),
			&decoded, nullptr), QStringLiteral("unknown file control type is rejected"));
		Check(!KFileTransferControlMessageCodec::decode(
			QStringLiteral("{\"version\":1,\"type\":\"fileTransferListDirectoryRequest\","
				"\"requestId\":\"550e8400-e29b-41d4-a716-446655440000\","
				"\"displayPath\":\"x\","
				"\"listingId\":\"123e4567-e89b-12d3-a456-426614174000\","
				"\"entryId\":\"16fd2706-8baf-433b-82eb-8c7fada847da\"}"),
			&decoded, nullptr), QStringLiteral("ambiguous directory selector is rejected"));

		KFileTransferControlMessage invalidDigest = MessageForType(
			FileCompleteFileTransferControlMessageType);
		invalidDigest.sha256 = QByteArray("short");
		Check(!KFileTransferControlMessageCodec::decode(
			KFileTransferControlMessageCodec::encode(invalidDigest), &decoded, nullptr),
			QStringLiteral("invalid file digest length is rejected"));
		KFileTransferControlMessage invalidPagination = MessageForType(
			ListDirectoryResponseFileTransferControlMessageType);
		invalidPagination.strNextPageToken.clear();
		Check(!KFileTransferControlMessageCodec::decode(
			KFileTransferControlMessageCodec::encode(invalidPagination), &decoded, nullptr),
			QStringLiteral("missing continuation token is rejected"));
		KFileTransferControlMessage duplicateCopy = MessageForType(
			CopyRequestFileTransferControlMessageType);
		duplicateCopy.entryIdList.append(duplicateCopy.entryIdList.first());
		Check(!KFileTransferControlMessageCodec::decode(
			KFileTransferControlMessageCodec::encode(duplicateCopy), &decoded, nullptr),
			QStringLiteral("duplicate copy entry ids are rejected"));
		KFileTransferControlMessage oversizedPlan = MessageForType(
			CopyPlanBeginFileTransferControlMessageType);
		oversizedPlan.nItemCount =
			KFileTransferControlMessageCodec::kMaximumCopyPlanItemCount + 1;
		Check(!KFileTransferControlMessageCodec::decode(
			KFileTransferControlMessageCodec::encode(oversizedPlan), &decoded, nullptr),
			QStringLiteral("oversized copy plan is rejected"));
		KFileTransferControlMessage completedPlan = MessageForType(
			CopyPlanEndFileTransferControlMessageType);
		completedPlan.taskResult = CompletedFileTransferTaskResult;
		Check(KFileTransferControlMessageCodec::decode(
			KFileTransferControlMessageCodec::encode(completedPlan), &decoded, nullptr)
			&& decoded.taskResult == CompletedFileTransferTaskResult,
			QStringLiteral("copy plan completion result round-trips"));

		KFileTransferControlMessage tooManyEntries = MessageForType(
			ListRootsResponseFileTransferControlMessageType);
		while (tooManyEntries.entryList.size()
			<= KFileTransferControlMessageCodec::kMaximumEntryCount)
		{
			KFileTransferEntry entry = tooManyEntries.entryList.first();
			entry.strEntryId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			tooManyEntries.entryList.append(entry);
		}
		Check(!KFileTransferControlMessageCodec::decode(
			KFileTransferControlMessageCodec::encode(tooManyEntries), &decoded, nullptr),
			QStringLiteral("oversized directory page is rejected"));
		Check(KFileTransferControlMessageCodec::conflictResolutionName(
				KeepBothFileTransferConflictResolution) == QStringLiteral("keepBoth")
			&& KFileTransferControlMessageCodec::taskResultName(
				SkippedFileTransferTaskResult) == QStringLiteral("skipped")
			&& KProtocolEnvelopeCodec::channelName(FileControlProtocolChannel)
				== QStringLiteral("file-control"),
			QStringLiteral("file transfer protocol names are stable"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestPermissionScopeNames();
	TestFileTransferCapabilities();
	TestLifecycleMessages();
	TestControlMessages();
	return g_nFailureCount == 0 ? 0 : 1;
}
