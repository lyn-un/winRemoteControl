#include "file_transfer/filetransfersessionservice.h"

#include "common/sessiontracelogger.h"
#include "core/file_transfer/filesystemport.h"
#include "core/protocol/protocolconstraints.h"
#include "session/sessioncontroller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QQueue>
#include <QtCore/QSet>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace
{
	constexpr int kOpenTimeoutMs = 5000;
	constexpr int kWorkerShutdownTimeoutMs = 2000;
	constexpr int kDataChunkBytes = 32 * 1024;
	constexpr quint64 kAcknowledgementIntervalBytes = 256 * 1024;
	constexpr quint64 kMaximumUnacknowledgedBytes = 1024 * 1024;
	constexpr int kMaximumListingEntries = 256;
	constexpr int kMaximumRetainedListings = 32;
	constexpr int kMaximumCopyPlanEntries = 50000;
	constexpr int kCopyPlanQueueBatchSize = 128;
	constexpr int kMaximumQueuedControlMessages = 1024;
	constexpr int kMaximumQueuedControlBytes = 4 * 1024 * 1024;
	constexpr qint64 kProgressPublishIntervalMs = 100;
	constexpr quint16 kEndOfFileDataFrameFlag = 0x0001;

	QString NewUuid()
	{
		return QUuid::createUuid().toString(QUuid::WithoutBraces);
	}

	bool IsConnectedState(KSessionState state)
	{
		return state == ConnectedSessionState || state == StreamingSessionState;
	}

	KFileTransferEntryType ProtocolEntryType(KFileListingEntryType type)
	{
		if (type == DriveFileListingEntryType)
			return RootFileTransferEntryType;
		if (type == DirectoryFileListingEntryType)
			return DirectoryFileTransferEntryType;
		if (type == RegularFileListingEntryType)
			return RegularFileTransferEntryType;
		return InvalidFileTransferEntryType;
	}

	KFileListingEntryType ListingEntryType(KFileTransferEntryType type)
	{
		if (type == RootFileTransferEntryType)
			return DriveFileListingEntryType;
		if (type == DirectoryFileTransferEntryType)
			return DirectoryFileListingEntryType;
		if (type == RegularFileTransferEntryType)
			return RegularFileListingEntryType;
		return InvalidFileListingEntryType;
	}

	KFileTransferPaneEntry PaneEntry(const KFileListingEntry &entry)
	{
		KFileTransferPaneEntry paneEntry;
		paneEntry.strEntryId = entry.strOpaqueId;
		paneEntry.strName = entry.strName;
		paneEntry.type = entry.type;
		paneEntry.nSize = entry.nSize;
		paneEntry.lastModifiedUtc = entry.lastModifiedUtc;
		paneEntry.bNavigable = entry.type == DriveFileListingEntryType
			|| entry.type == DirectoryFileListingEntryType;
		paneEntry.bTransferable = entry.type == DirectoryFileListingEntryType
			|| entry.type == RegularFileListingEntryType;
		return paneEntry;
	}

	KFileTransferPaneEntry PaneEntry(const KFileTransferEntry &entry)
	{
		KFileTransferPaneEntry paneEntry;
		paneEntry.strEntryId = entry.strEntryId;
		paneEntry.strName = entry.strName;
		paneEntry.type = ListingEntryType(entry.type);
		paneEntry.nSize = entry.nSize;
		paneEntry.lastModifiedUtc = QDateTime::fromMSecsSinceEpoch(
			entry.nModifiedAtMs, QTimeZone::UTC);
		paneEntry.bNavigable = entry.bNavigable;
		paneEntry.bTransferable = entry.bTransferable;
		return paneEntry;
	}

	KFileTransferEntry ProtocolEntry(const KFileListingEntry &entry)
	{
		KFileTransferEntry protocolEntry;
		protocolEntry.type = ProtocolEntryType(entry.type);
		protocolEntry.strEntryId = entry.strOpaqueId;
		protocolEntry.strName = entry.strName;
		protocolEntry.nSize = entry.nSize;
		protocolEntry.nModifiedAtMs = entry.lastModifiedUtc.isValid()
			? entry.lastModifiedUtc.toMSecsSinceEpoch() : 0;
		protocolEntry.bNavigable = entry.type == DriveFileListingEntryType
			|| entry.type == DirectoryFileListingEntryType;
		protocolEntry.bTransferable = entry.type == DirectoryFileListingEntryType
			|| entry.type == RegularFileListingEntryType;
		return protocolEntry;
	}

	QString ChildDisplayPath(const QString &strParent, const QString &strName)
	{
		if (strParent.isEmpty())
			return strName;
		if (strParent.endsWith(QLatin1Char('\\')))
			return strParent + strName;
		return strParent + QLatin1Char('\\') + strName;
	}

	QString RelativeChildPath(const QString &strParent, const QString &strName)
	{
		return strParent.isEmpty() ? strName
			: strParent + QLatin1Char('/') + strName;
	}

	QString RelativeParentPath(const QString &strPath)
	{
		const int nSeparator = strPath.lastIndexOf(QLatin1Char('/'));
		return nSeparator < 0 ? QString() : strPath.left(nSeparator);
	}

	QString RelativeFileName(const QString &strPath)
	{
		const int nSeparator = strPath.lastIndexOf(QLatin1Char('/'));
		return nSeparator < 0 ? strPath : strPath.mid(nSeparator + 1);
	}

	struct KFileSystemSharedState
	{
		explicit KFileSystemSharedState(std::unique_ptr<IKFileSystemPort> spPort)
			: spFileSystem(std::move(spPort))
		{
		}

		std::unique_ptr<IKFileSystemPort> spFileSystem;
		std::atomic<bool> bClosed = false;
	};

	struct KListingJobResult
	{
		bool bSuccess = false;
		QString strError;
		QString strDirectoryId;
		QString strDisplayPath;
		QVector<KFileListingEntry> entryList;
	};

	struct KSourceWorkerState
	{
		QString strSourceId;
		QCryptographicHash hash = QCryptographicHash(QCryptographicHash::Sha256);
	};

	struct KSourceOpenResult
	{
		bool bSuccess = false;
		QString strError;
		KFileSourceSnapshot snapshot;
		std::shared_ptr<KSourceWorkerState> spState;
	};

	struct KSourceReadResult
	{
		bool bSuccess = false;
		QString strError;
		QByteArray data;
		QByteArray sha256;
		bool bComplete = false;
	};

	struct KDestinationCheckResult
	{
		bool bSuccess = false;
		bool bExists = false;
		QString strError;
	};

	struct KWriteOpenResult
	{
		bool bSuccess = false;
		QString strError;
		KFileWriteSession session;
	};

	struct KWriteResult
	{
		bool bSuccess = false;
		QString strError;
		KFileWriteResult result;
	};

	struct KPlanSourceSelection
	{
		QString strOpaqueId;
		QString strName;
		KFileListingEntryType type = InvalidFileListingEntryType;
		quint64 nSize = 0;
		QDateTime lastModifiedUtc;
	};

	struct KPlanScanJobResult
	{
		bool bSuccess = false;
		QString strError;
		QStringList relativeDirectoryPathList;
		QVector<KFileTreeFileEntry> fileList;
		quint64 nTotalBytes = 0;
	};

	struct KDirectoryPreparationJobResult
	{
		bool bSuccess = false;
		QString strError;
		KDirectoryPreparationResult result;
	};

	struct KDirectoryCleanupJobResult
	{
		bool bSuccess = false;
		QString strError;
		KDirectoryCleanupResult result;
	};
}

class KFileTransferSessionServicePrivate
{
public:
	struct KOwnListing
	{
		QString strListingId;
		QString strDirectoryId;
		QString strDisplayPath;
		QVector<KFileListingEntry> entryList;
		bool bRoots = false;
	};

	struct KPaneContext
	{
		KFileTransferPaneSnapshot current;
		QVector<KFileTransferPaneSnapshot> history;
		QString strPendingRequestId;
		QString strPendingListingId;
		QString strPendingEntryId;
		QString strPendingDisplayPath;
		QString strResponseListingId;
		QString strResponseDisplayPath;
		QVector<KFileTransferEntry> accumulatedEntries;
		QSet<QString> seenPageTokenSet;
		int nReceivedPageCount = 0;
	};

	struct KTask
	{
		KFileTransferTaskSnapshot snapshot;
		QString strRequestId;
		QString strSourceListingId;
		QString strSourceEntryId;
		QStringList sourceEntryIdList;
		QString strDestinationListingId;
		QString strDestinationDirectoryId;
		QString strWriteId;
		QString strConflictId;
		QString strFileName;
		QString strRelativePath;
		QDateTime lastModifiedUtc;
		QByteArray expectedSha256;
		QByteArray pendingReadData;
		std::shared_ptr<KSourceWorkerState> spSourceState;
		quint64 nGeneration = 0;
		quint64 nPendingReadOffset = 0;
		quint64 nSentOffset = 0;
		quint64 nAcknowledgedOffset = 0;
		quint64 nQueuedReceiveOffset = 0;
		quint64 nWrittenOffset = 0;
		quint64 nLastAcknowledgedWriteOffset = 0;
		qint64 nLastProgressPublishedMs = 0;
		bool bSender = false;
		bool bReadPending = false;
		bool bPendingReadComplete = false;
		bool bWriteReady = false;
		bool bCompletionReceived = false;
		bool bCompletionSent = false;
		bool bFinalizing = false;
		bool bPausedForReconnect = false;
		bool bPlan = false;
		bool bPlanChild = false;
		QString strPlanTaskId;
		QString strPlanRequestId;
	};

	struct KCopyPlan
	{
		QString strTaskId;
		QString strRequestId;
		QString strDestinationListingId;
		QString strDestinationDirectoryId;
		KFileTransferDirection direction = InvalidFileTransferDirection;
		QStringList relativeDirectoryPathList;
		QVector<KFileTreeFileEntry> fileList;
		QHash<QString, QString> destinationDirectoryMap;
		QStringList createdDirectoryCleanupTokenList;
		QQueue<KFileTransferControlMessage> pendingFileBeginQueue;
		quint64 nItemCount = 0;
		quint64 nTotalBytes = 0;
		quint64 nReceivedBytes = 0;
		quint64 nGeneration = 0;
		int nExpectedFileCount = 0;
		int nReceivedFileCount = 0;
		int nCompletedFileCount = 0;
		bool bSender = false;
		bool bPlanBeginSent = false;
		bool bPlanEndSent = false;
		bool bChildrenQueued = false;
		int nNextDirectoryToSend = 0;
		int nNextFileToQueue = -1;
		KFileTransferConflictResolution defaultConflictResolution =
			InvalidFileTransferConflictResolution;
		bool bManifestEnded = false;
		bool bDirectoriesReady = false;
		bool bCompletionReceived = false;
		bool bHadSkippedFile = false;
	};

	struct KQueuedControlMessage
	{
		KFileTransferControlMessage message;
		int nEncodedBytes = 0;
	};

	explicit KFileTransferSessionServicePrivate(
		KFileTransferSessionService *pOwner,
		std::unique_ptr<IKFileSystemPort> spFileSystem,
		KSessionController *pSessionController)
		: m_pOwner(pOwner)
		, m_pSessionController(pSessionController)
		, m_spFileSystemState(std::make_shared<KFileSystemSharedState>(
			std::move(spFileSystem)))
		, m_pOpenTimer(new QTimer(pOwner))
		, m_pWorkerThread(new QThread())
		, m_pWorkerContext(new QObject())
	{
		Q_ASSERT(m_pOwner != nullptr);
		Q_ASSERT(m_pSessionController != nullptr);
		Q_ASSERT(m_spFileSystemState->spFileSystem != nullptr);

		m_pOpenTimer->setSingleShot(true);
		m_pOpenTimer->setInterval(kOpenTimeoutMs);
		QObject::connect(m_pOpenTimer, &QTimer::timeout, m_pOwner, [this]()
			{
				if (m_state != OpeningFileTransferState
					&& m_state != WaitingChannelsFileTransferState)
				{
					return;
				}
				failFeature(QStringLiteral("open_timeout"),
					QStringLiteral("File transfer open handshake timed out"));
			});

		m_pWorkerContext->moveToThread(m_pWorkerThread);
		QObject::connect(m_pWorkerThread, &QThread::finished,
			m_pWorkerContext, &QObject::deleteLater);
		QObject::connect(m_pWorkerThread, &QThread::finished,
			m_pWorkerThread, &QObject::deleteLater);
		m_pWorkerThread->start();

		m_localPane.current.pane = LocalFileTransferPane;
		m_remotePane.current.pane = RemoteFileTransferPane;
		connectController();
	}

	~KFileTransferSessionServicePrivate()
	{
		m_spFileSystemState->bClosed.store(true);
		if (m_pWorkerThread != nullptr && m_pWorkerThread->isRunning())
		{
			m_pWorkerThread->quit();
			m_pWorkerThread->wait(kWorkerShutdownTimeoutMs);
		}
	}

	KFileTransferState state() const
	{
		return m_state;
	}

	bool isAvailable() const
	{
		return IsConnectedState(m_sessionState)
			&& m_capabilities.bValid
			&& m_capabilities.bFileTransfer;
	}

	bool hasActiveTasks() const
	{
		for (const QString &strTaskId : m_rootTaskIdSet)
		{
			const auto iterator = m_taskMap.constFind(strTaskId);
			if (iterator == m_taskMap.cend())
				continue;
			const KTask &task = iterator.value();
			if (task.snapshot.state == ScanningFileTransferTaskState
				|| task.snapshot.state == QueuedFileTransferTaskState
				|| task.snapshot.state == TransferringFileTransferTaskState
				|| task.snapshot.state == PausedFileTransferTaskState
				|| task.snapshot.state == WaitingConflictFileTransferTaskState)
			{
				return true;
			}
		}
		return false;
	}

	void openCurrentFileTransfer();
	void stopByUser();
	void stopCurrentFileTransfer(bool bNotifyRemote = true, bool bControlledStop = false);
	void finishPendingStopIfReady();
	bool hasCommitInProgress() const;
	void requestSnapshot();
	void requestPaneRoots(KFileTransferPane pane);
	void navigatePane(KFileTransferPane pane,
		const QString &strListingId,
		const QString &strTargetEntryId);
	void navigatePaneByPath(KFileTransferPane pane, const QString &strAbsolutePath);
	void navigatePaneUp(KFileTransferPane pane, const QString &strListingId);
	void refreshPane(KFileTransferPane pane);
	void startCopy(KFileTransferPane sourcePane,
		const QString &strSourceListingId,
		const QStringList &sourceEntryIdList,
		const QString &strDestinationListingId);
	void pauseTask(const QString &strTaskId);
	void resumeTask(const QString &strTaskId);
	void cancelTask(const QString &strTaskId, bool bNotifyRemote = true);
	void retryTask(const QString &strTaskId);
	void resolveConflict(const QString &strConflictId,
		KFileTransferConflictResolution resolution,
		bool bApplyToRemaining);
	void clearCompletedTasks();
	void shutdown();

private:
	template<typename TResult, typename TWork, typename TComplete>
	void queueFileSystemJob(TWork work, TComplete complete)
	{
		if (m_bWorkerStopping || m_pWorkerContext == nullptr)
			return;
		++m_nPendingWorkerJobs;
		const std::shared_ptr<KFileSystemSharedState> spState = m_spFileSystemState;
		QPointer<KFileTransferSessionService> owner(m_pOwner);
		QMetaObject::invokeMethod(m_pWorkerContext,
			[spState, owner, work = std::move(work), complete = std::move(complete)]() mutable
			{
				if (spState->bClosed.load() || spState->spFileSystem == nullptr)
				{
					if (!owner.isNull())
					{
						QMetaObject::invokeMethod(owner, [owner]()
							{
								if (!owner.isNull())
									owner->m_spPrivate->workerJobFinished();
							}, Qt::QueuedConnection);
					}
					return;
				}
				auto spResult = std::make_shared<TResult>(work(spState->spFileSystem.get()));
				if (owner.isNull())
					return;
				QMetaObject::invokeMethod(owner,
					[owner, spResult, complete = std::move(complete)]() mutable
					{
						if (owner.isNull())
							return;
						complete(std::move(*spResult));
						owner->m_spPrivate->workerJobFinished();
					}, Qt::QueuedConnection);
			}, Qt::QueuedConnection);
	}

	void connectController();
	void handleSessionStateChanged(KSessionState state);
	void handleCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void handleLifecycleMessage(const KFileTransferLifecycleMessage &message);
	void handleControlMessage(const KFileTransferControlMessage &message);
	void handleData(const QByteArray &data);
	void handleChannelsChanged(bool bControlOpen, bool bDataOpen);
	void updateReadyState();
	void resetSession();
	void setState(KFileTransferState state, const QString &strStatusCode);
	void failFeature(const QString &strErrorCode, const QString &strTechnicalMessage);
	void writeTrace(const QString &strStage, const QString &strExtra = QString()) const;

	KPaneContext &pane(KFileTransferPane pane);
	const KPaneContext &pane(KFileTransferPane pane) const;
	void listLocalRoots(KFileTransferPane pane, bool bPublishToRemote,
		const QString &strRemoteRequestId = QString(),
		const QString &strPageToken = QString());
	void listLocalDirectory(KFileTransferPane pane,
		const QString &strDirectoryId,
		const QString &strDisplayPath,
		bool bPublishToRemote,
		const QString &strRemoteRequestId = QString(),
		const QString &strPageToken = QString());
	void listLocalPath(KFileTransferPane pane,
		const QString &strAbsolutePath,
		bool bPublishToRemote,
		const QString &strRemoteRequestId = QString());
	void publishLocalListing(KFileTransferPane pane,
		const KListingJobResult &result,
		const QString &strRequestId,
		bool bPublishToRemote,
		const QString &strPageToken);
	void sendListingPage(const KOwnListing &listing,
		KFileTransferControlMessageType responseType,
		const QString &strRequestId,
		int nOffset);
	void handleListingResponse(const KFileTransferControlMessage &message);
	void requestRemoteRoots(const QString &strPageToken = QString());
	void requestRemoteDirectory(const QString &strListingId,
		const QString &strEntryId,
		const QString &strDisplayPath,
		const QString &strPageToken = QString());
	void retainOwnListing(const KOwnListing &listing);
	const KOwnListing *ownListing(const QString &strListingId) const;
	const KFileListingEntry *ownEntry(const QString &strListingId,
		const QString &strEntryId) const;

	void startNextTask();
	void startPlanTask(const QString &strTaskId);
	void scanAndPublishPlan(const QString &strTaskId,
		const QVector<KPlanSourceSelection> &selectionList);
	void publishPlan(const QString &strTaskId, const KPlanScanJobResult &result);
	void pumpPlanManifest(const QString &strPlanTaskId);
	void queuePlanFiles(const QString &strPlanTaskId);
	void handlePlanBegin(const KFileTransferControlMessage &message);
	void handlePlanDirectory(const KFileTransferControlMessage &message);
	void handlePlanEnd(const KFileTransferControlMessage &message);
	void preparePlanDirectories(const QString &strPlanTaskId);
	void processPlanFileBegin(const KFileTransferControlMessage &message);
	void completePlanIfReady(const QString &strPlanTaskId);
	void finishPlan(const QString &strPlanTaskId, KFileTransferTaskResult result,
		bool bNotifyRemote);
	void cancelPlan(const QString &strPlanTaskId, bool bNotifyRemote);
	void cleanupPlanDirectories(const QString &strPlanTaskId);
	KCopyPlan *planForRequest(const QString &strRequestId);
	void startSenderTask(const QString &strTaskId);
	void openSource(const QString &strTaskId);
	void pumpSender(const QString &strTaskId);
	bool sendPendingSourceRead(KTask *pTask);
	void sendFileComplete(KTask *pTask, const QByteArray &sha256);
	void prepareReceiver(const KFileTransferControlMessage &message);
	void openDestination(const QString &strTaskId, KFileCollisionPolicy policy);
	void appendReceivedData(KTask *pTask, const KFileTransferDataFrame &frame);
	void tryFinalizeReceiver(const QString &strTaskId);
	void finishTask(const QString &strTaskId, KFileTransferTaskResult result);
	void failTask(const QString &strTaskId,
		const QString &strErrorCode,
		const QString &strTechnicalMessage,
		bool bNotifyRemote = true);
	bool requestWriteCancellation(KTask *pTask);
	void cleanupTaskResources(KTask *pTask);
	void emitTask(const KTask &task);
	void emitProgress(KTask *pTask, bool bForce = false);
	void emitActivity();
	bool sendControl(const KFileTransferControlMessage &message);
	bool enqueueControl(const KFileTransferControlMessage &message);
	void flushControlQueue();
	bool sendLifecycle(KFileTransferLifecycleMessageType type,
		const QString &strErrorCode = QString());
	void workerJobFinished();
	void finishShutdownIfReady();

	KFileTransferSessionService *m_pOwner = nullptr;
	KSessionController *m_pSessionController = nullptr;
	std::shared_ptr<KFileSystemSharedState> m_spFileSystemState;
	QTimer *m_pOpenTimer = nullptr;
	QPointer<QThread> m_pWorkerThread;
	QPointer<QObject> m_pWorkerContext;
	KFileTransferState m_state = ClosedFileTransferState;
	KSessionState m_sessionState = IdleSessionState;
	KNegotiatedCapabilities m_capabilities;
	KPaneContext m_localPane;
	KPaneContext m_remotePane;
	QHash<QString, KOwnListing> m_ownListingMap;
	QQueue<QString> m_ownListingOrder;
	QHash<QString, QPair<QString, int>> m_pageTokenMap;
	QHash<QString, KTask> m_taskMap;
	QSet<QString> m_rootTaskIdSet;
	QHash<QString, KCopyPlan> m_planMap;
	QHash<QString, QString> m_planTaskIdByRequestId;
	QQueue<QString> m_taskQueue;
	QQueue<KQueuedControlMessage> m_controlQueue;
	QString m_strActiveTaskId;
	QString m_strLifecycleRequestId;
	KFileTransferConflictResolution m_defaultConflictResolution =
		InvalidFileTransferConflictResolution;
	QString m_strStatusCode = QStringLiteral("closed");
	quint64 m_nGeneration = 0;
	quint64 m_nBlockedGeneration = 0;
	int m_nPendingWorkerJobs = 0;
	int m_nQueuedControlBytes = 0;
	bool m_bControlChannelOpen = false;
	bool m_bDataChannelOpen = false;
	bool m_bOpenAccepted = false;
	bool m_bShutdownRequested = false;
	bool m_bWorkerStopping = false;
	bool m_bShutdownFinishedEmitted = false;
	bool m_bStopPending = false;
	bool m_bResetAfterStop = false;
};

KFileTransferSessionService::KFileTransferSessionService(
	std::unique_ptr<IKFileSystemPort> spFileSystem,
	KSessionController *pSessionController,
	QObject *pParent)
	: QObject(pParent)
	, m_spPrivate(std::make_unique<KFileTransferSessionServicePrivate>(
		this, std::move(spFileSystem), pSessionController))
{
}

KFileTransferSessionService::~KFileTransferSessionService()
{
	m_spPrivate->shutdown();
}

KFileTransferState KFileTransferSessionService::state() const
{
	return m_spPrivate->state();
}

bool KFileTransferSessionService::isAvailable() const
{
	return m_spPrivate->isAvailable();
}

bool KFileTransferSessionService::hasActiveTasks() const
{
	return m_spPrivate->hasActiveTasks();
}

void KFileTransferSessionService::openCurrentFileTransfer()
{
	m_spPrivate->openCurrentFileTransfer();
}

void KFileTransferSessionService::stopCurrentFileTransfer()
{
	m_spPrivate->stopByUser();
}

void KFileTransferSessionService::requestSnapshot()
{
	m_spPrivate->requestSnapshot();
}

void KFileTransferSessionService::requestPaneRoots(KFileTransferPane pane)
{
	m_spPrivate->requestPaneRoots(pane);
}

void KFileTransferSessionService::navigatePane(KFileTransferPane pane,
	const QString &strListingId,
	const QString &strTargetEntryId)
{
	m_spPrivate->navigatePane(pane, strListingId, strTargetEntryId);
}

void KFileTransferSessionService::navigatePaneByPath(
	KFileTransferPane pane,
	const QString &strAbsolutePath)
{
	m_spPrivate->navigatePaneByPath(pane, strAbsolutePath);
}

void KFileTransferSessionService::navigatePaneUp(
	KFileTransferPane pane,
	const QString &strListingId)
{
	m_spPrivate->navigatePaneUp(pane, strListingId);
}

void KFileTransferSessionService::refreshPane(KFileTransferPane pane)
{
	m_spPrivate->refreshPane(pane);
}

void KFileTransferSessionService::startCopy(KFileTransferPane sourcePane,
	const QString &strSourceListingId,
	const QStringList &sourceEntryIdList,
	const QString &strDestinationListingId)
{
	m_spPrivate->startCopy(sourcePane, strSourceListingId,
		sourceEntryIdList, strDestinationListingId);
}

void KFileTransferSessionService::pauseTask(const QString &strTaskId)
{
	m_spPrivate->pauseTask(strTaskId);
}

void KFileTransferSessionService::resumeTask(const QString &strTaskId)
{
	m_spPrivate->resumeTask(strTaskId);
}

void KFileTransferSessionService::cancelTask(const QString &strTaskId)
{
	m_spPrivate->cancelTask(strTaskId);
}

void KFileTransferSessionService::retryTask(const QString &strTaskId)
{
	m_spPrivate->retryTask(strTaskId);
}

void KFileTransferSessionService::resolveConflict(
	const QString &strConflictId,
	KFileTransferConflictResolution resolution,
	bool bApplyToRemaining)
{
	m_spPrivate->resolveConflict(strConflictId, resolution, bApplyToRemaining);
}

void KFileTransferSessionService::clearCompletedTasks()
{
	m_spPrivate->clearCompletedTasks();
}

void KFileTransferSessionService::shutdown()
{
	m_spPrivate->shutdown();
}

void KFileTransferSessionServicePrivate::connectController()
{
	QObject::connect(m_pSessionController, &KSessionController::sessionStateChanged,
		m_pOwner, [this](KSessionState state)
		{
			handleSessionStateChanged(state);
		});
	QObject::connect(m_pSessionController, &KSessionController::sessionCapabilitiesChanged,
		m_pOwner, [this](const KNegotiatedCapabilities &capabilities)
		{
			handleCapabilitiesChanged(capabilities);
		});
	QObject::connect(m_pSessionController,
		&KSessionController::fileTransferLifecycleMessageReceived,
		m_pOwner, [this](const KFileTransferLifecycleMessage &message)
		{
			handleLifecycleMessage(message);
		});
	QObject::connect(m_pSessionController,
		&KSessionController::fileTransferControlMessageReceived,
		m_pOwner, [this](const KFileTransferControlMessage &message)
		{
			handleControlMessage(message);
		});
	QObject::connect(m_pSessionController, &KSessionController::fileTransferDataReceived,
		m_pOwner, [this](const QByteArray &data)
		{
			handleData(data);
		});
	QObject::connect(m_pSessionController, &KSessionController::fileTransferChannelsChanged,
		m_pOwner, [this](bool bControlOpen, bool bDataOpen)
		{
			handleChannelsChanged(bControlOpen, bDataOpen);
		});
	QObject::connect(m_pSessionController,
		&KSessionController::fileTransferLowWatermarkReached,
		m_pOwner, [this]()
		{
			flushControlQueue();
			if (!m_controlQueue.isEmpty())
				return;
			const QStringList planTaskIdList = m_planMap.keys();
			for (const QString &strPlanTaskId : planTaskIdList)
			{
				const auto iterator = m_planMap.constFind(strPlanTaskId);
				if (iterator != m_planMap.cend() && iterator->bSender
					&& !iterator->bManifestEnded)
				{
					pumpPlanManifest(strPlanTaskId);
				}
			}
			if (!m_strActiveTaskId.isEmpty())
				pumpSender(m_strActiveTaskId);
		});
}

void KFileTransferSessionServicePrivate::openCurrentFileTransfer()
{
	if (m_pSessionController->sessionRole() != ControllerSessionRole)
	{
		emit m_pOwner->transferError(QStringLiteral("controller_required"),
			QStringLiteral("Only the controller can open the file transfer window"));
		return;
	}
	if (!isAvailable())
	{
		emit m_pOwner->transferError(QStringLiteral("file_transfer_unavailable"),
			QStringLiteral("File transfer capability or permission is unavailable"));
		setState(FailedFileTransferState, QStringLiteral("unavailable"));
		return;
	}
	if (m_state == ReadyFileTransferState
		|| m_state == OpeningFileTransferState
		|| m_state == WaitingChannelsFileTransferState)
	{
		requestSnapshot();
		return;
	}

	m_nGeneration = m_pSessionController->sessionGeneration();
	m_strLifecycleRequestId = NewUuid();
	m_bOpenAccepted = false;
	setState(OpeningFileTransferState, QStringLiteral("opening"));
	if (!sendLifecycle(OpenRequestFileTransferLifecycleMessageType))
	{
		failFeature(QStringLiteral("open_send_failed"),
			QStringLiteral("Unable to send the file transfer open request"));
		return;
	}
	m_pOpenTimer->start();
	writeTrace(QStringLiteral("file_transfer_open_requested"));
}

void KFileTransferSessionServicePrivate::stopCurrentFileTransfer(
	bool bNotifyRemote,
	bool bControlledStop)
{
	if (m_state == ClosedFileTransferState && m_taskMap.isEmpty()
		&& !m_bStopPending)
		return;
	if (bControlledStop && m_pSessionController->sessionRole() == ControlledSessionRole)
		m_nBlockedGeneration = m_pSessionController->sessionGeneration();
	m_pOpenTimer->stop();
	setState(ClosingFileTransferState, QStringLiteral("closing"));
	if (bNotifyRemote && !m_strLifecycleRequestId.isEmpty())
	{
		sendLifecycle(bControlledStop
			? StoppedFileTransferLifecycleMessageType
			: CloseFileTransferLifecycleMessageType);
	}
	const QStringList taskIds = m_rootTaskIdSet.values();
	for (const QString &strTaskId : taskIds)
		cancelTask(strTaskId, bNotifyRemote);
	if (hasCommitInProgress())
	{
		m_bStopPending = true;
		setState(ClosingFileTransferState, QStringLiteral("waiting_for_commit"));
		return;
	}
	m_bStopPending = false;
	m_strActiveTaskId.clear();
	m_taskQueue.clear();
	m_bOpenAccepted = false;
	setState(ClosedFileTransferState, bControlledStop
		? QStringLiteral("stopped_by_controlled") : QStringLiteral("closed"));
	writeTrace(QStringLiteral("file_transfer_closed"),
		QStringLiteral("controlledStop=%1").arg(bControlledStop ? 1 : 0));
}

bool KFileTransferSessionServicePrivate::hasCommitInProgress() const
{
	for (const KTask &task : m_taskMap)
	{
		if (task.bFinalizing && !task.strWriteId.isEmpty())
			return true;
	}
	return false;
}

void KFileTransferSessionServicePrivate::finishPendingStopIfReady()
{
	if (!m_bStopPending || hasCommitInProgress())
		return;
	m_bStopPending = false;
	stopCurrentFileTransfer(false);
	if (m_bResetAfterStop)
	{
		m_bResetAfterStop = false;
		resetSession();
	}
}

void KFileTransferSessionServicePrivate::stopByUser()
{
	stopCurrentFileTransfer(true,
		m_pSessionController->sessionRole() == ControlledSessionRole);
}

void KFileTransferSessionServicePrivate::handleSessionStateChanged(KSessionState state)
{
	m_sessionState = state;
	if (state == ReconnectingSessionState)
	{
		if (m_state == ReadyFileTransferState)
			setState(ReconnectingFileTransferState, QStringLiteral("reconnecting"));
		for (auto iterator = m_taskMap.begin(); iterator != m_taskMap.end(); ++iterator)
		{
			if (iterator->snapshot.state == TransferringFileTransferTaskState)
			{
				iterator->bPausedForReconnect = true;
				iterator->snapshot.state = PausedFileTransferTaskState;
				emitTask(iterator.value());
			}
		}
		return;
	}
	if (IsConnectedState(state) && m_state == ReconnectingFileTransferState)
	{
		updateReadyState();
		return;
	}
	if (state == IdleSessionState || state == ListeningSessionState
		|| state == StoppingSessionState || state == ShutdownTimedOutSessionState)
	{
		m_bResetAfterStop = true;
		stopCurrentFileTransfer(false);
		if (!m_bStopPending)
		{
			m_bResetAfterStop = false;
			resetSession();
		}
	}
}

void KFileTransferSessionServicePrivate::handleCapabilitiesChanged(
	const KNegotiatedCapabilities &capabilities)
{
	m_capabilities = capabilities;
	if (!capabilities.bFileTransfer && m_state != ClosedFileTransferState)
		stopCurrentFileTransfer(false);
	else
		setState(m_state, m_strStatusCode);
}

void KFileTransferSessionServicePrivate::handleLifecycleMessage(
	const KFileTransferLifecycleMessage &message)
{
	if (message.nGeneration != m_pSessionController->sessionGeneration())
		return;
	if (message.type == OpenRequestFileTransferLifecycleMessageType)
	{
		if (m_pSessionController->sessionRole() != ControlledSessionRole
			|| !isAvailable()
			|| m_nBlockedGeneration == message.nGeneration)
		{
			m_strLifecycleRequestId = message.strRequestId;
			m_nGeneration = message.nGeneration;
			sendLifecycle(OpenRejectedFileTransferLifecycleMessageType,
				m_nBlockedGeneration == message.nGeneration
					? QStringLiteral("stopped_by_controlled")
					: QStringLiteral("permission_denied"));
			return;
		}
		m_nGeneration = message.nGeneration;
		m_strLifecycleRequestId = message.strRequestId;
		m_bOpenAccepted = true;
		setState(OpeningFileTransferState, QStringLiteral("opening"));
		if (!sendLifecycle(OpenAcceptedFileTransferLifecycleMessageType))
		{
			failFeature(QStringLiteral("accept_send_failed"),
				QStringLiteral("Unable to acknowledge the file transfer open request"));
			return;
		}
		m_pOpenTimer->start();
		updateReadyState();
		writeTrace(QStringLiteral("file_transfer_open_accepted"));
		return;
	}
	if (message.strRequestId != m_strLifecycleRequestId
		|| message.nGeneration != m_nGeneration)
	{
		return;
	}
	if (message.type == OpenAcceptedFileTransferLifecycleMessageType)
	{
		if (m_pSessionController->sessionRole() != ControllerSessionRole)
			return;
		m_bOpenAccepted = true;
		setState(WaitingChannelsFileTransferState, QStringLiteral("waiting_channels"));
		if (!m_pSessionController->ensureFileTransferChannels())
		{
			failFeature(QStringLiteral("channel_create_failed"),
				QStringLiteral("Unable to create file transfer data channels"));
			return;
		}
		updateReadyState();
	}
	else if (message.type == OpenRejectedFileTransferLifecycleMessageType
		|| message.type == ErrorFileTransferLifecycleMessageType)
	{
		failFeature(message.strErrorCode.isEmpty()
			? QStringLiteral("remote_rejected") : message.strErrorCode,
			QStringLiteral("The remote device rejected file transfer"));
	}
	else if (message.type == CloseFileTransferLifecycleMessageType)
	{
		stopCurrentFileTransfer(false);
		sendLifecycle(StoppedFileTransferLifecycleMessageType);
	}
	else if (message.type == StoppedFileTransferLifecycleMessageType)
	{
		stopCurrentFileTransfer(false);
		setState(ClosedFileTransferState, QStringLiteral("stopped_by_remote"));
	}
}

void KFileTransferSessionServicePrivate::handleChannelsChanged(
	bool bControlOpen,
	bool bDataOpen)
{
	const bool bWasReady = m_bControlChannelOpen && m_bDataChannelOpen;
	m_bControlChannelOpen = bControlOpen;
	m_bDataChannelOpen = bDataOpen;
	if (bControlOpen)
		flushControlQueue();
	if (bWasReady && (!bControlOpen || !bDataOpen)
		&& m_state != ClosedFileTransferState
		&& m_sessionState != ReconnectingSessionState)
	{
		failFeature(QStringLiteral("channel_closed"),
			QStringLiteral("A file transfer data channel closed"));
		return;
	}
	updateReadyState();
}

void KFileTransferSessionServicePrivate::updateReadyState()
{
	if (!m_bOpenAccepted || !isAvailable()
		|| !m_bControlChannelOpen || !m_bDataChannelOpen)
	{
		return;
	}
	m_pOpenTimer->stop();
	setState(ReadyFileTransferState, QStringLiteral("ready"));
	for (auto iterator = m_taskMap.begin(); iterator != m_taskMap.end(); ++iterator)
	{
		if (iterator->bPausedForReconnect
			&& iterator->snapshot.state == PausedFileTransferTaskState)
		{
			iterator->bPausedForReconnect = false;
			iterator->snapshot.state = TransferringFileTransferTaskState;
			emitTask(iterator.value());
		}
	}
	if (m_pSessionController->sessionRole() == ControllerSessionRole
		&& m_localPane.current.strListingId.isEmpty())
	{
		requestPaneRoots(LocalFileTransferPane);
		requestPaneRoots(RemoteFileTransferPane);
	}
	if (!m_strActiveTaskId.isEmpty())
		pumpSender(m_strActiveTaskId);
	writeTrace(QStringLiteral("file_transfer_ready"));
}

void KFileTransferSessionServicePrivate::resetSession()
{
	m_pOpenTimer->stop();
	m_nGeneration = 0;
	m_strLifecycleRequestId.clear();
	m_bOpenAccepted = false;
	m_bControlChannelOpen = false;
	m_bDataChannelOpen = false;
	m_capabilities = KNegotiatedCapabilities();
	m_localPane = KPaneContext();
	m_localPane.current.pane = LocalFileTransferPane;
	m_remotePane = KPaneContext();
	m_remotePane.current.pane = RemoteFileTransferPane;
	m_ownListingMap.clear();
	m_ownListingOrder.clear();
	m_pageTokenMap.clear();
	m_taskMap.clear();
	m_rootTaskIdSet.clear();
	m_planMap.clear();
	m_planTaskIdByRequestId.clear();
	m_taskQueue.clear();
	m_controlQueue.clear();
	m_nQueuedControlBytes = 0;
	m_strActiveTaskId.clear();
	m_defaultConflictResolution = InvalidFileTransferConflictResolution;
	m_bStopPending = false;
	m_bResetAfterStop = false;
	setState(ClosedFileTransferState, QStringLiteral("closed"));
	requestSnapshot();
}

void KFileTransferSessionServicePrivate::setState(
	KFileTransferState state,
	const QString &strStatusCode)
{
	m_state = state;
	m_strStatusCode = strStatusCode;
	emit m_pOwner->stateChanged(state, isAvailable(), strStatusCode);
	emitActivity();
}

void KFileTransferSessionServicePrivate::failFeature(
	const QString &strErrorCode,
	const QString &strTechnicalMessage)
{
	m_pOpenTimer->stop();
	writeTrace(QStringLiteral("file_transfer_failed"),
		QStringLiteral("code=%1").arg(strErrorCode));
	emit m_pOwner->transferError(strErrorCode, strTechnicalMessage);
	setState(FailedFileTransferState, strErrorCode);
	const QStringList taskIds = m_taskMap.keys();
	for (const QString &strTaskId : taskIds)
		failTask(strTaskId, strErrorCode, strTechnicalMessage, false);
}

void KFileTransferSessionServicePrivate::writeTrace(
	const QString &strStage,
	const QString &strExtra) const
{
	KSessionTraceLogger::write(
		m_pSessionController->sessionRole() == ControllerSessionRole
			? QStringLiteral("controller") : QStringLiteral("controlled"),
		strStage, QStringLiteral("file_transfer"), -1,
		QStringLiteral("generation=%1 requestId=%2 %3")
			.arg(m_pSessionController->sessionGeneration())
			.arg(m_strLifecycleRequestId, strExtra));
}

KFileTransferSessionServicePrivate::KPaneContext &
KFileTransferSessionServicePrivate::pane(KFileTransferPane pane)
{
	return pane == RemoteFileTransferPane ? m_remotePane : m_localPane;
}

const KFileTransferSessionServicePrivate::KPaneContext &
KFileTransferSessionServicePrivate::pane(KFileTransferPane pane) const
{
	return pane == RemoteFileTransferPane ? m_remotePane : m_localPane;
}

void KFileTransferSessionServicePrivate::requestSnapshot()
{
	QVector<KFileTransferTaskSnapshot> taskList;
	taskList.reserve(m_rootTaskIdSet.size());
	for (const QString &strTaskId : m_rootTaskIdSet)
	{
		const auto iterator = m_taskMap.constFind(strTaskId);
		if (iterator != m_taskMap.cend())
			taskList.append(iterator->snapshot);
	}
	emit m_pOwner->snapshotChanged(taskList);
	emit m_pOwner->paneChanged(m_localPane.current);
	emit m_pOwner->paneChanged(m_remotePane.current);
	setState(m_state, m_strStatusCode);

	// The snapshot event clears transient UI state. Re-emit an outstanding
	// conflict afterwards so reopening the window restores the active prompt.
	if (m_pSessionController->sessionRole() != ControllerSessionRole)
		return;
	for (const KTask &task : m_taskMap)
	{
		if (task.snapshot.state != WaitingConflictFileTransferTaskState
			|| task.strConflictId.isEmpty())
		{
			continue;
		}
		KFileTransferConflictSnapshot conflict;
		conflict.strConflictId = task.strConflictId;
		conflict.strTaskId = task.snapshot.strTaskId;
		conflict.strFileId = task.snapshot.strFileId;
		conflict.strName = task.strFileName;
		conflict.nSourceSize = task.snapshot.nBytesTotal;
		conflict.sourceLastModifiedUtc = task.lastModifiedUtc;
		conflict.bApplyToRemainingAllowed = true;
		emit m_pOwner->conflictRequested(conflict);
		break;
	}
}

void KFileTransferSessionServicePrivate::requestPaneRoots(KFileTransferPane paneType)
{
	if (m_state != ReadyFileTransferState)
		return;
	if (paneType == LocalFileTransferPane)
		listLocalRoots(paneType, false);
	else if (m_pSessionController->sessionRole() == ControllerSessionRole)
		requestRemoteRoots();
}

void KFileTransferSessionServicePrivate::navigatePane(
	KFileTransferPane paneType,
	const QString &strListingId,
	const QString &strTargetEntryId)
{
	if (m_state != ReadyFileTransferState)
		return;
	KPaneContext &context = pane(paneType);
	if (context.current.strListingId != strListingId)
	{
		emit m_pOwner->transferError(QStringLiteral("stale_listing"),
			QStringLiteral("The selected directory listing is no longer current"));
		return;
	}
	const auto iterator = std::find_if(context.current.entryList.cbegin(),
		context.current.entryList.cend(),
		[&strTargetEntryId](const KFileTransferPaneEntry &entry)
		{
			return entry.strEntryId == strTargetEntryId;
		});
	if (iterator == context.current.entryList.cend() || !iterator->bNavigable)
	{
		emit m_pOwner->transferError(QStringLiteral("invalid_directory_entry"),
			QStringLiteral("The selected entry is not a navigable directory"));
		return;
	}
	context.history.append(context.current);
	const QString strDisplayPath = ChildDisplayPath(
		context.current.strDisplayPath, iterator->strName);
	if (paneType == LocalFileTransferPane)
		listLocalDirectory(paneType, iterator->strEntryId, strDisplayPath, false);
	else
		requestRemoteDirectory(strListingId, strTargetEntryId, QString());
}

void KFileTransferSessionServicePrivate::navigatePaneByPath(
	KFileTransferPane paneType,
	const QString &strAbsolutePath)
{
	if (m_state != ReadyFileTransferState || strAbsolutePath.isEmpty())
		return;
	KPaneContext &context = pane(paneType);
	if (!context.current.strListingId.isEmpty())
		context.history.append(context.current);
	if (paneType == LocalFileTransferPane)
		listLocalPath(paneType, strAbsolutePath, false);
	else
		requestRemoteDirectory(QString(), QString(), strAbsolutePath);
}

void KFileTransferSessionServicePrivate::navigatePaneUp(
	KFileTransferPane paneType,
	const QString &strListingId)
{
	KPaneContext &context = pane(paneType);
	if (context.current.strListingId != strListingId || context.history.isEmpty())
		return;
	context.current = context.history.takeLast();
	emit m_pOwner->paneChanged(context.current);
}

void KFileTransferSessionServicePrivate::refreshPane(KFileTransferPane paneType)
{
	KPaneContext &context = pane(paneType);
	if (context.current.strListingId.isEmpty()
		|| context.current.strDirectoryId.isEmpty())
	{
		requestPaneRoots(paneType);
		return;
	}
	if (paneType == LocalFileTransferPane)
	{
		listLocalDirectory(paneType, context.current.strDirectoryId,
			context.current.strDisplayPath, false);
	}
	else
	{
		requestRemoteDirectory(QString(), QString(),
			context.current.strDisplayPath);
	}
}

void KFileTransferSessionServicePrivate::listLocalRoots(
	KFileTransferPane paneType,
	bool bPublishToRemote,
	const QString &strRemoteRequestId,
	const QString &strPageToken)
{
	const QString strRequestId = strRemoteRequestId.isEmpty()
		? NewUuid() : strRemoteRequestId;
	if (!bPublishToRemote)
	{
		pane(paneType).strPendingRequestId = strRequestId;
		emit m_pOwner->paneLoading(paneType, strRequestId);
	}
	queueFileSystemJob<KListingJobResult>(
		[](IKFileSystemPort *pFileSystem)
		{
			KListingJobResult result;
			result.bSuccess = pFileSystem->listDrives(
				&result.entryList, &result.strError);
			return result;
		},
		[this, paneType, bPublishToRemote, strRequestId,
			strPageToken,
			nGeneration = m_nGeneration](KListingJobResult result)
		{
			if (nGeneration != m_nGeneration)
				return;
			publishLocalListing(paneType, result, strRequestId,
				bPublishToRemote, strPageToken);
		});
}

void KFileTransferSessionServicePrivate::listLocalDirectory(
	KFileTransferPane paneType,
	const QString &strDirectoryId,
	const QString &strDisplayPath,
	bool bPublishToRemote,
	const QString &strRemoteRequestId,
	const QString &strPageToken)
{
	const QString strRequestId = strRemoteRequestId.isEmpty()
		? NewUuid() : strRemoteRequestId;
	if (!bPublishToRemote)
	{
		pane(paneType).strPendingRequestId = strRequestId;
		emit m_pOwner->paneLoading(paneType, strRequestId);
	}
	queueFileSystemJob<KListingJobResult>(
		[strDirectoryId, strDisplayPath](IKFileSystemPort *pFileSystem)
		{
			KListingJobResult result;
			result.strDirectoryId = strDirectoryId;
			result.strDisplayPath = strDisplayPath;
			result.bSuccess = pFileSystem->listDirectory(strDirectoryId,
				&result.entryList, &result.strError);
			return result;
		},
		[this, paneType, bPublishToRemote, strRequestId,
			strPageToken,
			nGeneration = m_nGeneration](KListingJobResult result)
		{
			if (nGeneration != m_nGeneration)
				return;
			publishLocalListing(paneType, result, strRequestId,
				bPublishToRemote, strPageToken);
		});
}

void KFileTransferSessionServicePrivate::listLocalPath(
	KFileTransferPane paneType,
	const QString &strAbsolutePath,
	bool bPublishToRemote,
	const QString &strRemoteRequestId)
{
	const QString strRequestId = strRemoteRequestId.isEmpty()
		? NewUuid() : strRemoteRequestId;
	if (!bPublishToRemote)
	{
		pane(paneType).strPendingRequestId = strRequestId;
		emit m_pOwner->paneLoading(paneType, strRequestId);
	}
	queueFileSystemJob<KListingJobResult>(
		[strAbsolutePath](IKFileSystemPort *pFileSystem)
		{
			KListingJobResult result;
			KFileListingEntry directory;
			result.bSuccess = pFileSystem->createLocalReference(
				strAbsolutePath, &directory, &result.strError);
			if (!result.bSuccess)
				return result;
			if (directory.type != DriveFileListingEntryType
				&& directory.type != DirectoryFileListingEntryType)
			{
				result.bSuccess = false;
				result.strError = QStringLiteral("The requested path is not a directory");
				return result;
			}
			result.strDirectoryId = directory.strOpaqueId;
			result.strDisplayPath = strAbsolutePath;
			result.bSuccess = pFileSystem->listDirectory(directory.strOpaqueId,
				&result.entryList, &result.strError);
			return result;
		},
		[this, paneType, bPublishToRemote, strRequestId,
			nGeneration = m_nGeneration](KListingJobResult result)
		{
			if (nGeneration != m_nGeneration)
				return;
			publishLocalListing(paneType, result, strRequestId,
				bPublishToRemote, QString());
		});
}

void KFileTransferSessionServicePrivate::publishLocalListing(
	KFileTransferPane paneType,
	const KListingJobResult &result,
	const QString &strRequestId,
	bool bPublishToRemote,
	const QString &strPageToken)
{
	if (!result.bSuccess)
	{
		if (bPublishToRemote)
		{
			KFileTransferControlMessage error;
			error.type = ErrorFileTransferControlMessageType;
			error.strRequestId = strRequestId;
			error.strErrorCode = QStringLiteral("list_failed");
			sendControl(error);
		}
		else
		{
			emit m_pOwner->transferError(QStringLiteral("list_failed"), result.strError);
		}
		return;
	}

	KOwnListing listing;
	listing.strListingId = NewUuid();
	listing.strDirectoryId = result.strDirectoryId;
	listing.strDisplayPath = result.strDisplayPath;
	listing.entryList = result.entryList;
	listing.bRoots = result.strDirectoryId.isEmpty();
	retainOwnListing(listing);
	if (bPublishToRemote)
	{
		sendListingPage(listing, listing.bRoots
			? ListRootsResponseFileTransferControlMessageType
			: ListDirectoryResponseFileTransferControlMessageType,
			strRequestId, 0);
		return;
	}

	KPaneContext &context = pane(paneType);
	if (context.strPendingRequestId != strRequestId)
		return;
	KFileTransferPaneSnapshot snapshot;
	snapshot.pane = paneType;
	snapshot.strRequestId = strRequestId;
	snapshot.strListingId = listing.strListingId;
	snapshot.strDirectoryId = listing.strDirectoryId;
	snapshot.strDisplayPath = listing.strDisplayPath;
	snapshot.bCanGoUp = !context.history.isEmpty();
	for (const KFileListingEntry &entry : listing.entryList)
		snapshot.entryList.append(PaneEntry(entry));
	context.current = snapshot;
	emit m_pOwner->paneChanged(snapshot);
}

void KFileTransferSessionServicePrivate::sendListingPage(
	const KOwnListing &listing,
	KFileTransferControlMessageType responseType,
	const QString &strRequestId,
	int nOffset)
{
	if (nOffset < 0 || nOffset > listing.entryList.size())
		return;
	KFileTransferControlMessage response;
	response.type = responseType;
	response.strRequestId = strRequestId;
	response.strListingId = listing.strListingId;
	response.strDisplayPath = listing.strDisplayPath;
	response.bCanGoUp = !listing.bRoots;
	int nEnd = nOffset;
	while (nEnd < listing.entryList.size()
		&& nEnd - nOffset < kMaximumListingEntries)
	{
		response.entryList.append(ProtocolEntry(listing.entryList.at(nEnd)));
		const QByteArray encoded = KFileTransferControlMessageCodec::encode(
			response).toUtf8();
		if (encoded.isEmpty()
			|| encoded.size()
				> KProtocolConstraints::kMaximumFileControlMessageBytes - 512)
		{
			response.entryList.removeLast();
			break;
		}
		++nEnd;
	}
	if (nEnd == nOffset && nOffset < listing.entryList.size())
	{
		KFileTransferControlMessage error;
		error.type = ErrorFileTransferControlMessageType;
		error.strRequestId = strRequestId;
		error.strErrorCode = QStringLiteral("listing_entry_too_large");
		sendControl(error);
		return;
	}
	response.bHasMore = nEnd < listing.entryList.size();
	if (response.bHasMore)
	{
		response.strNextPageToken = NewUuid();
		m_pageTokenMap.insert(response.strNextPageToken,
			qMakePair(listing.strListingId, nEnd));
	}
	sendControl(response);
}

void KFileTransferSessionServicePrivate::requestRemoteRoots(
	const QString &strPageToken)
{
	KFileTransferControlMessage request;
	request.type = ListRootsRequestFileTransferControlMessageType;
	request.strRequestId = strPageToken.isEmpty()
		? NewUuid() : m_remotePane.strPendingRequestId;
	request.strNextPageToken = strPageToken;
	if (strPageToken.isEmpty())
	{
		m_remotePane.strPendingRequestId = request.strRequestId;
		m_remotePane.accumulatedEntries.clear();
		m_remotePane.seenPageTokenSet.clear();
		m_remotePane.nReceivedPageCount = 0;
		m_remotePane.strResponseListingId.clear();
		m_remotePane.strResponseDisplayPath.clear();
		emit m_pOwner->paneLoading(RemoteFileTransferPane, request.strRequestId);
	}
	sendControl(request);
}

void KFileTransferSessionServicePrivate::requestRemoteDirectory(
	const QString &strListingId,
	const QString &strEntryId,
	const QString &strDisplayPath,
	const QString &strPageToken)
{
	KFileTransferControlMessage request;
	request.type = ListDirectoryRequestFileTransferControlMessageType;
	request.strRequestId = m_remotePane.strPendingRequestId.isEmpty()
		|| strPageToken.isEmpty() ? NewUuid() : m_remotePane.strPendingRequestId;
	request.strListingId = strListingId;
	request.strEntryId = strEntryId;
	request.strDisplayPath = strDisplayPath;
	request.strNextPageToken = strPageToken;
	if (strPageToken.isEmpty())
	{
		m_remotePane.strPendingRequestId = request.strRequestId;
		m_remotePane.strPendingListingId = strListingId;
		m_remotePane.strPendingEntryId = strEntryId;
		m_remotePane.strPendingDisplayPath = strDisplayPath;
		m_remotePane.accumulatedEntries.clear();
		m_remotePane.seenPageTokenSet.clear();
		m_remotePane.nReceivedPageCount = 0;
		m_remotePane.strResponseListingId.clear();
		m_remotePane.strResponseDisplayPath.clear();
		emit m_pOwner->paneLoading(RemoteFileTransferPane, request.strRequestId);
	}
	sendControl(request);
}

void KFileTransferSessionServicePrivate::handleListingResponse(
	const KFileTransferControlMessage &message)
{
	if (m_pSessionController->sessionRole() != ControllerSessionRole
		|| message.strRequestId != m_remotePane.strPendingRequestId)
	{
		return;
	}
	if (++m_remotePane.nReceivedPageCount
		> (kMaximumCopyPlanEntries + kMaximumListingEntries - 1)
			/ kMaximumListingEntries
		|| m_remotePane.accumulatedEntries.size() + message.entryList.size()
			> kMaximumCopyPlanEntries)
	{
		m_remotePane.accumulatedEntries.clear();
		m_remotePane.strPendingRequestId.clear();
		emit m_pOwner->transferError(QStringLiteral("listing_too_large"),
			QStringLiteral("The remote directory contains too many entries"));
		return;
	}
	if (m_remotePane.strResponseListingId.isEmpty())
	{
		m_remotePane.strResponseListingId = message.strListingId;
		m_remotePane.strResponseDisplayPath = message.strDisplayPath;
	}
	else if (m_remotePane.strResponseListingId != message.strListingId
		|| m_remotePane.strResponseDisplayPath != message.strDisplayPath)
	{
		m_remotePane.accumulatedEntries.clear();
		m_remotePane.strPendingRequestId.clear();
		emit m_pOwner->transferError(QStringLiteral("listing_changed"),
			QStringLiteral("The remote listing changed while it was paged"));
		return;
	}
	m_remotePane.accumulatedEntries += message.entryList;
	if (message.bHasMore)
	{
		if (message.strNextPageToken.isEmpty()
			|| m_remotePane.seenPageTokenSet.contains(message.strNextPageToken))
		{
			m_remotePane.accumulatedEntries.clear();
			m_remotePane.strPendingRequestId.clear();
			emit m_pOwner->transferError(QStringLiteral("invalid_page_token"),
				QStringLiteral("The remote listing repeated a page token"));
			return;
		}
		m_remotePane.seenPageTokenSet.insert(message.strNextPageToken);
		if (message.type == ListRootsResponseFileTransferControlMessageType)
			requestRemoteRoots(message.strNextPageToken);
		else
		{
			requestRemoteDirectory(m_remotePane.strPendingListingId,
				m_remotePane.strPendingEntryId,
				m_remotePane.strPendingDisplayPath,
				message.strNextPageToken);
		}
		return;
	}

	KFileTransferPaneSnapshot snapshot;
	snapshot.pane = RemoteFileTransferPane;
	snapshot.strRequestId = message.strRequestId;
	snapshot.strListingId = m_remotePane.strResponseListingId;
	snapshot.strDirectoryId = message.type == ListDirectoryResponseFileTransferControlMessageType
		? m_remotePane.strResponseListingId : QString();
	snapshot.strDisplayPath = m_remotePane.strResponseDisplayPath;
	snapshot.bCanGoUp = !m_remotePane.history.isEmpty();
	for (const KFileTransferEntry &entry : std::as_const(m_remotePane.accumulatedEntries))
		snapshot.entryList.append(PaneEntry(entry));
	m_remotePane.current = snapshot;
	m_remotePane.accumulatedEntries.clear();
	m_remotePane.strPendingRequestId.clear();
	m_remotePane.seenPageTokenSet.clear();
	emit m_pOwner->paneChanged(snapshot);
}

void KFileTransferSessionServicePrivate::retainOwnListing(
	const KOwnListing &listing)
{
	m_ownListingMap.insert(listing.strListingId, listing);
	m_ownListingOrder.enqueue(listing.strListingId);
	while (m_ownListingOrder.size() > kMaximumRetainedListings)
	{
		const QString strExpired = m_ownListingOrder.dequeue();
		m_ownListingMap.remove(strExpired);
	}
}

const KFileTransferSessionServicePrivate::KOwnListing *
KFileTransferSessionServicePrivate::ownListing(const QString &strListingId) const
{
	const auto iterator = m_ownListingMap.constFind(strListingId);
	return iterator == m_ownListingMap.cend() ? nullptr : &iterator.value();
}

const KFileListingEntry *KFileTransferSessionServicePrivate::ownEntry(
	const QString &strListingId,
	const QString &strEntryId) const
{
	const KOwnListing *pListing = ownListing(strListingId);
	if (pListing == nullptr)
		return nullptr;
	const auto iterator = std::find_if(pListing->entryList.cbegin(),
		pListing->entryList.cend(), [&strEntryId](const KFileListingEntry &entry)
		{
			return entry.strOpaqueId == strEntryId;
		});
	return iterator == pListing->entryList.cend() ? nullptr : &(*iterator);
}

void KFileTransferSessionServicePrivate::startCopy(
	KFileTransferPane sourcePane,
	const QString &strSourceListingId,
	const QStringList &sourceEntryIdList,
	const QString &strDestinationListingId)
{
	if (m_state != ReadyFileTransferState
		|| m_pSessionController->sessionRole() != ControllerSessionRole)
	{
		emit m_pOwner->transferError(QStringLiteral("transfer_not_ready"),
			QStringLiteral("File transfer is not ready"));
		return;
	}
	if (sourceEntryIdList.isEmpty())
	{
		emit m_pOwner->transferError(QStringLiteral("empty_selection"),
			QStringLiteral("Select at least one file or directory"));
		return;
	}
	if (sourceEntryIdList.size()
		> KFileTransferControlMessageCodec::kMaximumEntryIdCount
		|| sourceEntryIdList.size() > kMaximumCopyPlanEntries)
	{
		emit m_pOwner->transferError(QStringLiteral("selection_too_large"),
			QStringLiteral("The selected item list exceeds the transfer limit"));
		return;
	}
	if (strSourceListingId.isEmpty() || strDestinationListingId.isEmpty()
		|| strSourceListingId.size()
			> KFileTransferControlMessageCodec::kMaximumOpaqueTokenCharacters
		|| strDestinationListingId.size()
			> KFileTransferControlMessageCodec::kMaximumOpaqueTokenCharacters)
	{
		emit m_pOwner->transferError(QStringLiteral("invalid_listing"),
			QStringLiteral("The source or destination listing identifier is invalid"));
		return;
	}
	for (const QString &strEntryId : sourceEntryIdList)
	{
		if (strEntryId.isEmpty()
			|| strEntryId.size()
				> KFileTransferControlMessageCodec::kMaximumOpaqueTokenCharacters)
		{
			emit m_pOwner->transferError(QStringLiteral("invalid_entry"),
				QStringLiteral("A selected entry identifier is invalid"));
			return;
		}
	}
	m_defaultConflictResolution = InvalidFileTransferConflictResolution;
	const KPaneContext &sourceContext = pane(sourcePane);
	const KPaneContext &destinationContext = pane(sourcePane == LocalFileTransferPane
		? RemoteFileTransferPane : LocalFileTransferPane);
	if (sourceContext.current.strListingId != strSourceListingId
		|| destinationContext.current.strListingId != strDestinationListingId
		|| destinationContext.current.strDirectoryId.isEmpty())
	{
		emit m_pOwner->transferError(QStringLiteral("stale_listing"),
			QStringLiteral("The source or destination listing is no longer current"));
		return;
	}

	QVector<KFileTransferPaneEntry> selectedEntries;
	selectedEntries.reserve(sourceEntryIdList.size());
	bool bRequiresPlan = sourceEntryIdList.size() > 1;
	for (const QString &strEntryId : sourceEntryIdList)
	{
		const auto entryIterator = std::find_if(
			sourceContext.current.entryList.cbegin(),
			sourceContext.current.entryList.cend(),
			[&strEntryId](const KFileTransferPaneEntry &entry)
			{
				return entry.strEntryId == strEntryId;
			});
		if (entryIterator == sourceContext.current.entryList.cend()
			|| (entryIterator->type != RegularFileListingEntryType
				&& entryIterator->type != DirectoryFileListingEntryType)
			|| !entryIterator->bTransferable)
		{
			emit m_pOwner->transferError(QStringLiteral("unsupported_entry"),
				QStringLiteral("The selected entry cannot be transferred"));
			return;
		}
		selectedEntries.append(*entryIterator);
		bRequiresPlan = bRequiresPlan
			|| entryIterator->type == DirectoryFileListingEntryType;
	}

	if (bRequiresPlan)
	{
		KTask task;
		task.snapshot.strTaskId = NewUuid();
		task.snapshot.strDisplayName = selectedEntries.size() == 1
			? selectedEntries.first().strName
			: QStringLiteral("%1 items").arg(selectedEntries.size());
		task.snapshot.kind = selectedEntries.size() == 1
			? FolderFileTransferTaskKind : BatchFileTransferTaskKind;
		task.snapshot.direction = sourcePane == LocalFileTransferPane
			? UploadFileTransferDirection : DownloadFileTransferDirection;
		task.snapshot.state = QueuedFileTransferTaskState;
		task.snapshot.bCanPause = true;
		task.strRequestId = NewUuid();
		task.strSourceListingId = strSourceListingId;
		task.sourceEntryIdList = sourceEntryIdList;
		task.strDestinationListingId = strDestinationListingId;
		task.bSender = sourcePane == LocalFileTransferPane;
		task.bPlan = true;
		task.nGeneration = m_nGeneration;
		const QString strTaskId = task.snapshot.strTaskId;
		m_taskMap.insert(strTaskId, task);
		m_rootTaskIdSet.insert(strTaskId);
		m_taskQueue.enqueue(strTaskId);
		emitTask(m_taskMap.value(strTaskId));
		startNextTask();
		return;
	}

	const KFileTransferPaneEntry &entry = selectedEntries.first();
	KTask task;
	task.snapshot.strTaskId = NewUuid();
	if (sourcePane == LocalFileTransferPane)
		task.snapshot.strFileId = NewUuid();
	task.snapshot.strDisplayName = entry.strName;
	task.snapshot.kind = FileFileTransferTaskKind;
	task.snapshot.direction = sourcePane == LocalFileTransferPane
		? UploadFileTransferDirection : DownloadFileTransferDirection;
	task.snapshot.state = QueuedFileTransferTaskState;
	task.snapshot.nBytesTotal = entry.nSize;
	task.snapshot.bCanPause = true;
	task.strRequestId = NewUuid();
	task.strSourceListingId = strSourceListingId;
	task.strSourceEntryId = entry.strEntryId;
	task.strDestinationListingId = strDestinationListingId;
	task.strFileName = entry.strName;
	task.strRelativePath = entry.strName;
	task.lastModifiedUtc = entry.lastModifiedUtc;
	task.bSender = sourcePane == LocalFileTransferPane;
	task.nGeneration = m_nGeneration;
	const QString strTaskId = task.snapshot.strTaskId;
	m_taskMap.insert(strTaskId, task);
	m_rootTaskIdSet.insert(strTaskId);
	m_taskQueue.enqueue(strTaskId);
	emitTask(m_taskMap.value(strTaskId));
	startNextTask();
}

void KFileTransferSessionServicePrivate::startNextTask()
{
	if (m_state != ReadyFileTransferState || !m_strActiveTaskId.isEmpty()
		|| m_bStopPending)
		return;
	while (!m_taskQueue.isEmpty())
	{
		const QString strTaskId = m_taskQueue.dequeue();
		auto iterator = m_taskMap.find(strTaskId);
		if (iterator == m_taskMap.end()
			|| iterator->snapshot.state != QueuedFileTransferTaskState)
		{
			continue;
		}
		m_strActiveTaskId = strTaskId;
		if (iterator->bPlan)
		{
			startPlanTask(strTaskId);
			return;
		}
		if (iterator->bSender)
		{
			if (iterator->bPlanChild)
			{
				startSenderTask(strTaskId);
				return;
			}
			KFileTransferControlMessage request;
			request.type = CopyRequestFileTransferControlMessageType;
			request.strRequestId = iterator->strRequestId;
			request.strTaskId = iterator->snapshot.strTaskId;
			request.strSourceListingId = iterator->strSourceListingId;
			request.strDestinationListingId = iterator->strDestinationListingId;
			request.entryIdList.append(iterator->strSourceEntryId);
			request.direction = UploadFileTransferDirection;
			if (!sendControl(request))
			{
				failTask(strTaskId, QStringLiteral("copy_request_failed"),
					QStringLiteral("Unable to authorize the remote destination"), false);
				return;
			}
			startSenderTask(strTaskId);
		}
		else
		{
			KFileTransferControlMessage request;
			request.type = CopyRequestFileTransferControlMessageType;
			request.strRequestId = iterator->strRequestId;
			request.strTaskId = iterator->snapshot.strTaskId;
			request.strSourceListingId = iterator->strSourceListingId;
			request.strDestinationListingId = iterator->strDestinationListingId;
			request.entryIdList.append(iterator->strSourceEntryId);
			request.direction = DownloadFileTransferDirection;
			iterator->snapshot.state = ScanningFileTransferTaskState;
			emitTask(iterator.value());
			if (!sendControl(request))
			{
				failTask(strTaskId, QStringLiteral("copy_request_failed"),
					QStringLiteral("Unable to request the remote source file"), false);
			}
		}
		return;
	}
}

void KFileTransferSessionServicePrivate::startPlanTask(const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end() || !iterator->bPlan)
		return;
	KTask &task = iterator.value();
	task.snapshot.state = ScanningFileTransferTaskState;
	emitTask(task);
	if (!task.bSender)
	{
		KFileTransferControlMessage request;
		request.type = CopyRequestFileTransferControlMessageType;
		request.strRequestId = task.strRequestId;
		request.strTaskId = task.snapshot.strTaskId;
		request.strSourceListingId = task.strSourceListingId;
		request.strDestinationListingId = task.strDestinationListingId;
		request.entryIdList = task.sourceEntryIdList;
		request.direction = DownloadFileTransferDirection;
		if (!sendControl(request))
		{
			failTask(strTaskId, QStringLiteral("copy_request_failed"),
				QStringLiteral("Unable to request the remote directory tree"), false);
		}
		return;
	}

	if (m_pSessionController->sessionRole() == ControllerSessionRole)
	{
		KFileTransferControlMessage request;
		request.type = CopyRequestFileTransferControlMessageType;
		request.strRequestId = task.strRequestId;
		request.strTaskId = task.snapshot.strTaskId;
		request.strSourceListingId = task.strSourceListingId;
		request.strDestinationListingId = task.strDestinationListingId;
		request.entryIdList = task.sourceEntryIdList;
		request.direction = UploadFileTransferDirection;
		if (!sendControl(request))
		{
			failTask(strTaskId, QStringLiteral("copy_request_failed"),
				QStringLiteral("Unable to authorize the remote copy plan"), false);
			return;
		}
	}

	QVector<KPlanSourceSelection> selectionList;
	const KOwnListing *pListing = ownListing(task.strSourceListingId);
	if (pListing == nullptr)
	{
		failTask(strTaskId, QStringLiteral("stale_source"),
			QStringLiteral("The selected source listing has expired"));
		return;
	}
	for (const QString &strEntryId : std::as_const(task.sourceEntryIdList))
	{
		const KFileListingEntry *pEntry = ownEntry(task.strSourceListingId, strEntryId);
		if (pEntry == nullptr
			|| (pEntry->type != RegularFileListingEntryType
				&& pEntry->type != DirectoryFileListingEntryType))
		{
			failTask(strTaskId, QStringLiteral("stale_source"),
				QStringLiteral("A selected source entry has expired"));
			return;
		}
		KPlanSourceSelection selection;
		selection.strOpaqueId = pEntry->strOpaqueId;
		selection.strName = pEntry->strName;
		selection.type = pEntry->type;
		selection.nSize = pEntry->nSize;
		selection.lastModifiedUtc = pEntry->lastModifiedUtc;
		selectionList.append(selection);
	}
	scanAndPublishPlan(strTaskId, selectionList);
}

void KFileTransferSessionServicePrivate::scanAndPublishPlan(
	const QString &strTaskId,
	const QVector<KPlanSourceSelection> &selectionList)
{
	queueFileSystemJob<KPlanScanJobResult>(
		[selectionList](IKFileSystemPort *pFileSystem)
		{
			KPlanScanJobResult result;
			QSet<QString> normalizedRelativePathSet;
			int nItemCount = 0;
			for (const KPlanSourceSelection &selection : selectionList)
			{
				if (selection.type == RegularFileListingEntryType)
				{
					++nItemCount;
					if (nItemCount > kMaximumCopyPlanEntries
						|| result.nTotalBytes
							> std::numeric_limits<quint64>::max() - selection.nSize)
					{
						result.strError = QStringLiteral("Copy plan exceeds its limits");
						return result;
					}
					const QString strKey = selection.strName.toCaseFolded();
					if (normalizedRelativePathSet.contains(strKey))
					{
						result.strError = QStringLiteral("Copy plan contains duplicate paths");
						return result;
					}
					normalizedRelativePathSet.insert(strKey);
					KFileTreeFileEntry file;
					file.strRelativePath = selection.strName;
					file.strOpaqueId = selection.strOpaqueId;
					file.nSize = selection.nSize;
					file.lastModifiedUtc = selection.lastModifiedUtc;
					result.fileList.append(file);
					result.nTotalBytes += selection.nSize;
					continue;
				}

				KFileTreeExpansion expansion;
				const int nRemaining = kMaximumCopyPlanEntries - nItemCount - 1;
				if (nRemaining < 0
					|| !pFileSystem->expandDirectoryTree(selection.strOpaqueId,
						qMax(1, nRemaining), &expansion, &result.strError))
				{
					if (result.strError.isEmpty())
						result.strError = QStringLiteral("Copy plan exceeds its limits");
					return result;
				}
				nItemCount += 1 + expansion.relativeDirectoryPathList.size()
					+ expansion.fileList.size();
				if (nItemCount > kMaximumCopyPlanEntries)
				{
					result.strError = QStringLiteral("Copy plan exceeds its limits");
					return result;
				}
				const QString strRootKey = selection.strName.toCaseFolded();
				if (normalizedRelativePathSet.contains(strRootKey))
				{
					result.strError = QStringLiteral("Copy plan contains duplicate paths");
					return result;
				}
				normalizedRelativePathSet.insert(strRootKey);
				result.relativeDirectoryPathList.append(selection.strName);
				for (const QString &strDirectory :
					std::as_const(expansion.relativeDirectoryPathList))
				{
					const QString strRelative = RelativeChildPath(
						selection.strName, strDirectory);
					const QString strKey = strRelative.toCaseFolded();
					if (normalizedRelativePathSet.contains(strKey))
					{
						result.strError = QStringLiteral("Copy plan contains duplicate paths");
						return result;
					}
					normalizedRelativePathSet.insert(strKey);
					result.relativeDirectoryPathList.append(strRelative);
				}
				for (KFileTreeFileEntry file : expansion.fileList)
				{
					file.strRelativePath = RelativeChildPath(
						selection.strName, file.strRelativePath);
					const QString strKey = file.strRelativePath.toCaseFolded();
					if (normalizedRelativePathSet.contains(strKey)
						|| result.nTotalBytes
							> std::numeric_limits<quint64>::max() - file.nSize)
					{
						result.strError = QStringLiteral("Copy plan is invalid or too large");
						return result;
					}
					normalizedRelativePathSet.insert(strKey);
					result.nTotalBytes += file.nSize;
					result.fileList.append(file);
				}
			}
			result.bSuccess = nItemCount > 0;
			if (!result.bSuccess && result.strError.isEmpty())
				result.strError = QStringLiteral("Copy plan is empty");
			return result;
		},
		[this, strTaskId, nGeneration = m_nGeneration](KPlanScanJobResult result)
		{
			if (nGeneration != m_nGeneration)
				return;
			publishPlan(strTaskId, result);
		});
}

void KFileTransferSessionServicePrivate::publishPlan(
	const QString &strTaskId,
	const KPlanScanJobResult &result)
{
	auto taskIterator = m_taskMap.find(strTaskId);
	if (taskIterator == m_taskMap.end() || !taskIterator->bPlan
		|| taskIterator->snapshot.state != ScanningFileTransferTaskState)
	{
		return;
	}
	if (!result.bSuccess)
	{
		failTask(strTaskId, QStringLiteral("directory_scan_failed"), result.strError);
		return;
	}

	KCopyPlan plan;
	plan.strTaskId = strTaskId;
	plan.strRequestId = taskIterator->strRequestId;
	plan.strDestinationListingId = taskIterator->strDestinationListingId;
	plan.direction = taskIterator->snapshot.direction;
	plan.relativeDirectoryPathList = result.relativeDirectoryPathList;
	plan.fileList = result.fileList;
	plan.nItemCount = static_cast<quint64>(result.relativeDirectoryPathList.size())
		+ static_cast<quint64>(result.fileList.size());
	plan.nTotalBytes = result.nTotalBytes;
	plan.nGeneration = m_nGeneration;
	plan.nExpectedFileCount = result.fileList.size();
	plan.nNextFileToQueue = result.fileList.size() - 1;
	plan.bSender = true;
	plan.bDirectoriesReady = true;
	m_planMap.insert(strTaskId, plan);
	m_planTaskIdByRequestId.insert(plan.strRequestId, strTaskId);

	taskIterator->snapshot.nBytesTotal = result.nTotalBytes;
	taskIterator->snapshot.state = TransferringFileTransferTaskState;
	emitTask(taskIterator.value());
	pumpPlanManifest(strTaskId);
}

void KFileTransferSessionServicePrivate::pumpPlanManifest(
	const QString &strPlanTaskId)
{
	auto iterator = m_planMap.find(strPlanTaskId);
	if (iterator == m_planMap.end() || !iterator->bSender
		|| iterator->bManifestEnded || m_sessionState == ReconnectingSessionState)
	{
		return;
	}
	KCopyPlan &plan = iterator.value();
	if (!plan.bPlanBeginSent)
	{
		KFileTransferControlMessage begin;
		begin.type = CopyPlanBeginFileTransferControlMessageType;
		begin.strRequestId = plan.strRequestId;
		begin.strTaskId = plan.strTaskId;
		begin.strDestinationListingId = plan.strDestinationListingId;
		begin.direction = plan.direction;
		begin.nItemCount = plan.nItemCount;
		begin.nSize = plan.nTotalBytes;
		if (!sendControl(begin))
		{
			if (!m_pSessionController->isFileTransferBackpressured())
			{
				failTask(strPlanTaskId, QStringLiteral("plan_send_failed"),
					QStringLiteral("Unable to send the copy plan"), false);
			}
			return;
		}
		plan.bPlanBeginSent = true;
	}
	while (plan.nNextDirectoryToSend < plan.relativeDirectoryPathList.size())
	{
		if (!m_controlQueue.isEmpty()
			|| m_pSessionController->isFileTransferBackpressured())
			return;
		KFileTransferControlMessage directory;
		directory.type = CopyPlanDirectoryFileTransferControlMessageType;
		directory.strRequestId = plan.strRequestId;
		directory.strTaskId = plan.strTaskId;
		directory.strRelativePath = plan.relativeDirectoryPathList.at(
			plan.nNextDirectoryToSend);
		directory.nModifiedAtMs = 0;
		if (!sendControl(directory))
		{
			if (!m_pSessionController->isFileTransferBackpressured())
			{
				failTask(strPlanTaskId, QStringLiteral("plan_send_failed"),
					QStringLiteral("Unable to send the copy plan directories"), false);
			}
			return;
		}
		++plan.nNextDirectoryToSend;
	}
	if (!plan.bPlanEndSent)
	{
		KFileTransferControlMessage end;
		end.type = CopyPlanEndFileTransferControlMessageType;
		end.strRequestId = plan.strRequestId;
		end.strTaskId = plan.strTaskId;
		if (!sendControl(end))
		{
			if (!m_pSessionController->isFileTransferBackpressured())
			{
				failTask(strPlanTaskId, QStringLiteral("plan_send_failed"),
					QStringLiteral("Unable to finish the copy plan"), false);
			}
			return;
		}
		plan.bPlanEndSent = true;
	}
	plan.bManifestEnded = true;
	queuePlanFiles(strPlanTaskId);
}

void KFileTransferSessionServicePrivate::queuePlanFiles(
	const QString &strPlanTaskId)
{
	auto planIterator = m_planMap.find(strPlanTaskId);
	if (planIterator == m_planMap.end() || planIterator->bChildrenQueued)
		return;
	KCopyPlan &plan = planIterator.value();
	int nQueued = 0;
	while (plan.nNextFileToQueue >= 0 && nQueued < kCopyPlanQueueBatchSize)
	{
		const KFileTreeFileEntry &file = plan.fileList.at(plan.nNextFileToQueue);
		KTask child;
		child.snapshot.strTaskId = NewUuid();
		child.snapshot.strFileId = NewUuid();
		child.snapshot.strDisplayName = RelativeFileName(file.strRelativePath);
		child.snapshot.direction = plan.direction;
		child.snapshot.state = QueuedFileTransferTaskState;
		child.snapshot.nBytesTotal = file.nSize;
		child.snapshot.bCanPause = true;
		child.strRequestId = plan.strRequestId;
		child.strSourceEntryId = file.strOpaqueId;
		child.strDestinationListingId = plan.strDestinationListingId;
		child.strFileName = RelativeFileName(file.strRelativePath);
		child.strRelativePath = file.strRelativePath;
		child.lastModifiedUtc = file.lastModifiedUtc;
		child.bSender = true;
		child.bPlanChild = true;
		child.nGeneration = plan.nGeneration;
		child.strPlanTaskId = plan.strTaskId;
		child.strPlanRequestId = plan.strRequestId;
		const QString strChildTaskId = child.snapshot.strTaskId;
		m_taskMap.insert(strChildTaskId, child);
		m_taskQueue.prepend(strChildTaskId);
		--plan.nNextFileToQueue;
		++nQueued;
	}
	if (plan.nNextFileToQueue >= 0)
	{
		QPointer<KFileTransferSessionService> owner(m_pOwner);
		QTimer::singleShot(0, m_pOwner, [owner, strPlanTaskId]()
			{
				if (!owner.isNull())
					owner->m_spPrivate->queuePlanFiles(strPlanTaskId);
			});
		return;
	}
	plan.bChildrenQueued = true;
	if (m_strActiveTaskId == strPlanTaskId)
		m_strActiveTaskId.clear();
	startNextTask();
}

void KFileTransferSessionServicePrivate::handlePlanBegin(
	const KFileTransferControlMessage &message)
{
	const KSessionRole role = m_pSessionController->sessionRole();
	const KSessionRole expectedRole = message.direction == UploadFileTransferDirection
		? ControlledSessionRole : ControllerSessionRole;
	auto taskIterator = m_taskMap.find(message.strTaskId);
	const KOwnListing *pDestination = ownListing(message.strDestinationListingId);
	if (role != expectedRole
		|| taskIterator == m_taskMap.end()
		|| taskIterator->bSender
		|| taskIterator->strRequestId != message.strRequestId
		|| taskIterator->strDestinationListingId != message.strDestinationListingId
		|| taskIterator->snapshot.direction != message.direction
		|| taskIterator->snapshot.state != ScanningFileTransferTaskState
		|| m_planMap.contains(message.strTaskId)
		|| m_planTaskIdByRequestId.contains(message.strRequestId)
		|| pDestination == nullptr
		|| pDestination->strDirectoryId.isEmpty()
		|| message.nItemCount == 0
		|| message.nItemCount > static_cast<quint64>(kMaximumCopyPlanEntries))
	{
		KFileTransferControlMessage error;
		error.type = ErrorFileTransferControlMessageType;
		error.strRequestId = message.strRequestId;
		error.strTaskId = message.strTaskId;
		error.strErrorCode = QStringLiteral("invalid_copy_plan");
		sendControl(error);
		return;
	}

	KCopyPlan plan;
	plan.strTaskId = message.strTaskId;
	plan.strRequestId = message.strRequestId;
	plan.strDestinationListingId = message.strDestinationListingId;
	plan.strDestinationDirectoryId = pDestination->strDirectoryId;
	plan.direction = message.direction;
	plan.nItemCount = message.nItemCount;
	plan.nTotalBytes = message.nSize;
	plan.nGeneration = m_nGeneration;
	plan.bSender = false;
	m_planMap.insert(plan.strTaskId, plan);
	m_planTaskIdByRequestId.insert(plan.strRequestId, plan.strTaskId);
	taskIterator->bPlan = true;
	taskIterator->snapshot.kind = taskIterator->sourceEntryIdList.size() == 1
		? FolderFileTransferTaskKind : BatchFileTransferTaskKind;
	taskIterator->snapshot.nBytesTotal = plan.nTotalBytes;
	taskIterator->snapshot.state = ScanningFileTransferTaskState;
	emitTask(taskIterator.value());
}

void KFileTransferSessionServicePrivate::handlePlanDirectory(
	const KFileTransferControlMessage &message)
{
	auto planIterator = m_planMap.find(message.strTaskId);
	if (planIterator == m_planMap.end()
		|| planIterator->bSender
		|| planIterator->bManifestEnded
		|| planIterator->strRequestId != message.strRequestId
		|| static_cast<quint64>(planIterator->relativeDirectoryPathList.size())
			>= planIterator->nItemCount)
	{
		return;
	}
	const QString strKey = message.strRelativePath.toCaseFolded();
	for (const QString &strExisting :
		std::as_const(planIterator->relativeDirectoryPathList))
	{
		if (strExisting.toCaseFolded() == strKey)
		{
			failTask(message.strTaskId, QStringLiteral("duplicate_plan_path"),
				QStringLiteral("The copy plan contains a duplicate directory"));
			return;
		}
	}
	planIterator->relativeDirectoryPathList.append(message.strRelativePath);
}

void KFileTransferSessionServicePrivate::handlePlanEnd(
	const KFileTransferControlMessage &message)
{
	auto planIterator = m_planMap.find(message.strTaskId);
	if (planIterator == m_planMap.end()
		|| planIterator->strRequestId != message.strRequestId)
	{
		return;
	}
	if (message.taskResult != InvalidFileTransferTaskResult)
	{
		if (!planIterator->bSender || planIterator->bCompletionReceived)
			return;
		planIterator->bCompletionReceived = true;
		finishPlan(message.strTaskId, message.taskResult, false);
		return;
	}
	if (planIterator->bSender || planIterator->bManifestEnded)
		return;
	const quint64 nDirectoryCount = static_cast<quint64>(
		planIterator->relativeDirectoryPathList.size());
	if (nDirectoryCount > planIterator->nItemCount)
	{
		failTask(message.strTaskId, QStringLiteral("invalid_copy_plan"),
			QStringLiteral("The copy plan item count is inconsistent"));
		return;
	}
	planIterator->nExpectedFileCount = static_cast<int>(
		planIterator->nItemCount - nDirectoryCount);
	planIterator->bManifestEnded = true;
	preparePlanDirectories(message.strTaskId);
}

void KFileTransferSessionServicePrivate::preparePlanDirectories(
	const QString &strPlanTaskId)
{
	auto planIterator = m_planMap.find(strPlanTaskId);
	if (planIterator == m_planMap.end() || planIterator->bSender
		|| !planIterator->bManifestEnded || planIterator->bDirectoriesReady)
	{
		return;
	}
	const QString strRootDirectoryId = planIterator->strDestinationDirectoryId;
	const QStringList relativeDirectoryPathList =
		planIterator->relativeDirectoryPathList;
	queueFileSystemJob<KDirectoryPreparationJobResult>(
		[strRootDirectoryId, relativeDirectoryPathList](IKFileSystemPort *pFileSystem)
		{
			KDirectoryPreparationJobResult jobResult;
			jobResult.bSuccess = pFileSystem->prepareRelativeDirectories(
				strRootDirectoryId, relativeDirectoryPathList,
				&jobResult.result, &jobResult.strError);
			return jobResult;
		},
		[this, strPlanTaskId,
			nGeneration = m_nGeneration](KDirectoryPreparationJobResult jobResult)
		{
			auto current = m_planMap.find(strPlanTaskId);
			if (nGeneration != m_nGeneration || current == m_planMap.end()
				|| current->nGeneration != nGeneration || current->bSender)
			{
				if (jobResult.bSuccess && !jobResult.result.createdDirectoryList.isEmpty())
				{
					QStringList cleanupTokenList;
					for (const KCreatedDirectoryToken &created :
						std::as_const(jobResult.result.createdDirectoryList))
					{
						cleanupTokenList.append(created.strCleanupToken);
					}
					queueFileSystemJob<KDirectoryCleanupJobResult>(
						[cleanupTokenList](IKFileSystemPort *pFileSystem)
						{
							KDirectoryCleanupJobResult cleanup;
							cleanup.bSuccess = pFileSystem->cleanupCreatedDirectories(
								cleanupTokenList, &cleanup.result, &cleanup.strError);
							return cleanup;
						}, [](KDirectoryCleanupJobResult) {});
				}
				return;
			}
			if (!jobResult.bSuccess)
			{
				failTask(strPlanTaskId, QStringLiteral("directory_prepare_failed"),
					jobResult.strError);
				return;
			}
			current->destinationDirectoryMap.insert(QString(),
				current->strDestinationDirectoryId);
			for (const KRelativeDirectoryEntry &directory :
				std::as_const(jobResult.result.directoryList))
			{
				current->destinationDirectoryMap.insert(
					directory.strRelativePath, directory.strDirectoryOpaqueId);
			}
			for (const KCreatedDirectoryToken &created :
				std::as_const(jobResult.result.createdDirectoryList))
			{
				current->createdDirectoryCleanupTokenList.append(
					created.strCleanupToken);
			}
			for (const QString &strDirectory :
				std::as_const(current->relativeDirectoryPathList))
			{
				if (!current->destinationDirectoryMap.contains(strDirectory))
				{
					failTask(strPlanTaskId, QStringLiteral("directory_prepare_failed"),
						QStringLiteral("A destination directory was not prepared"));
					return;
				}
			}
			current->bDirectoriesReady = true;
			auto planTaskIterator = m_taskMap.find(strPlanTaskId);
			if (planTaskIterator != m_taskMap.end())
			{
				planTaskIterator->snapshot.state = TransferringFileTransferTaskState;
				emitTask(planTaskIterator.value());
			}
			while (!current->pendingFileBeginQueue.isEmpty())
			{
				const KFileTransferControlMessage pending =
					current->pendingFileBeginQueue.dequeue();
				processPlanFileBegin(pending);
				current = m_planMap.find(strPlanTaskId);
				if (current == m_planMap.end())
					return;
			}
			completePlanIfReady(strPlanTaskId);
		});
}

void KFileTransferSessionServicePrivate::processPlanFileBegin(
	const KFileTransferControlMessage &message)
{
	KCopyPlan *pPlan = planForRequest(message.strRequestId);
	if (pPlan == nullptr || pPlan->bSender
		|| pPlan->strDestinationListingId != message.strDestinationListingId
		|| pPlan->strTaskId == message.strTaskId
		|| !pPlan->bManifestEnded)
	{
		KFileTransferControlMessage error;
		error.type = ErrorFileTransferControlMessageType;
		error.strRequestId = message.strRequestId;
		error.strTaskId = message.strTaskId;
		error.strFileId = message.strFileId;
		error.strErrorCode = QStringLiteral("unknown_or_replayed_task");
		sendControl(error);
		return;
	}
	if (!pPlan->bDirectoriesReady)
	{
		if (pPlan->nReceivedFileCount + pPlan->pendingFileBeginQueue.size()
			>= pPlan->nExpectedFileCount)
		{
			failTask(pPlan->strTaskId, QStringLiteral("invalid_copy_plan"),
				QStringLiteral("The copy plan contains too many files"));
			return;
		}
		for (const KFileTransferControlMessage &pending :
			std::as_const(pPlan->pendingFileBeginQueue))
		{
			if (pending.strTaskId == message.strTaskId
				|| pending.strFileId == message.strFileId
				|| pending.strRelativePath.compare(message.strRelativePath,
					Qt::CaseInsensitive) == 0)
			{
				return;
			}
		}
		pPlan->pendingFileBeginQueue.enqueue(message);
		return;
	}
	if (pPlan->nReceivedFileCount >= pPlan->nExpectedFileCount
		|| m_taskMap.contains(message.strTaskId)
		|| message.strFileName != RelativeFileName(message.strRelativePath)
		|| !pPlan->destinationDirectoryMap.contains(
			RelativeParentPath(message.strRelativePath))
		|| message.nSize > pPlan->nTotalBytes
		|| pPlan->nReceivedBytes > pPlan->nTotalBytes - message.nSize)
	{
		failTask(pPlan->strTaskId, QStringLiteral("invalid_copy_plan"),
			QStringLiteral("The copy plan file metadata is invalid"));
		return;
	}
	for (auto iterator = m_taskMap.cbegin(); iterator != m_taskMap.cend(); ++iterator)
	{
		if (iterator->bPlanChild && iterator->strPlanTaskId == pPlan->strTaskId
			&& (iterator->snapshot.strFileId == message.strFileId
				|| iterator->strRelativePath.compare(message.strRelativePath,
					Qt::CaseInsensitive) == 0))
		{
			failTask(pPlan->strTaskId, QStringLiteral("duplicate_plan_path"),
				QStringLiteral("The copy plan contains duplicate file metadata"));
			return;
		}
	}

	KTask child;
	child.snapshot.strTaskId = message.strTaskId;
	child.snapshot.direction = pPlan->direction;
	child.snapshot.state = ScanningFileTransferTaskState;
	child.snapshot.nBytesTotal = message.nSize;
	child.snapshot.bCanPause = true;
	child.strRequestId = message.strRequestId;
	child.strDestinationListingId = message.strDestinationListingId;
	child.strDestinationDirectoryId = pPlan->destinationDirectoryMap.value(
		RelativeParentPath(message.strRelativePath));
	child.strFileName = message.strFileName;
	child.strRelativePath = message.strRelativePath;
	child.lastModifiedUtc = QDateTime::fromMSecsSinceEpoch(
		message.nModifiedAtMs, QTimeZone::UTC);
	child.bSender = false;
	child.bPlanChild = true;
	child.nGeneration = m_nGeneration;
	child.strPlanTaskId = pPlan->strTaskId;
	child.strPlanRequestId = pPlan->strRequestId;
	m_taskMap.insert(message.strTaskId, child);
	++pPlan->nReceivedFileCount;
	pPlan->nReceivedBytes += message.nSize;
	prepareReceiver(message);
}

void KFileTransferSessionServicePrivate::completePlanIfReady(
	const QString &strPlanTaskId)
{
	auto iterator = m_planMap.find(strPlanTaskId);
	if (iterator == m_planMap.end() || iterator->bSender
		|| !iterator->bManifestEnded || !iterator->bDirectoriesReady
		|| iterator->nReceivedFileCount != iterator->nExpectedFileCount
		|| iterator->nCompletedFileCount != iterator->nExpectedFileCount)
	{
		return;
	}
	if (iterator->nReceivedBytes != iterator->nTotalBytes)
	{
		failTask(strPlanTaskId, QStringLiteral("invalid_copy_plan"),
			QStringLiteral("The copy plan byte count is inconsistent"));
		return;
	}
	finishPlan(strPlanTaskId,
		iterator->bHadSkippedFile ? SkippedFileTransferTaskResult
			: CompletedFileTransferTaskResult, true);
}

void KFileTransferSessionServicePrivate::finishPlan(
	const QString &strPlanTaskId,
	KFileTransferTaskResult result,
	bool bNotifyRemote)
{
	auto planIterator = m_planMap.find(strPlanTaskId);
	auto taskIterator = m_taskMap.find(strPlanTaskId);
	if (planIterator == m_planMap.end() || taskIterator == m_taskMap.end())
		return;
	const QString strRequestId = planIterator->strRequestId;
	if (bNotifyRemote)
	{
		KFileTransferControlMessage complete;
		complete.type = CopyPlanEndFileTransferControlMessageType;
		complete.strRequestId = strRequestId;
		complete.strTaskId = strPlanTaskId;
		complete.taskResult = result;
		if (!sendControl(complete))
		{
			failTask(strPlanTaskId, QStringLiteral("plan_complete_failed"),
				QStringLiteral("Unable to publish copy plan completion"), false);
			return;
		}
	}
	KTask &task = taskIterator.value();
	task.snapshot.state = result == SkippedFileTransferTaskResult
		? CancelledFileTransferTaskState : CompletedFileTransferTaskState;
	task.snapshot.nBytesTransferred = result == CompletedFileTransferTaskResult
		? task.snapshot.nBytesTotal : task.snapshot.nBytesTransferred;
	task.snapshot.bCanPause = false;
	task.snapshot.bCanRetry = result == SkippedFileTransferTaskResult;
	emitTask(task);
	m_planTaskIdByRequestId.remove(strRequestId);
	m_planMap.erase(planIterator);
	if (m_strActiveTaskId == strPlanTaskId)
		m_strActiveTaskId.clear();
	startNextTask();
	emitActivity();
}

void KFileTransferSessionServicePrivate::cancelPlan(
	const QString &strPlanTaskId,
	bool bNotifyRemote)
{
	auto planIterator = m_planMap.find(strPlanTaskId);
	auto taskIterator = m_taskMap.find(strPlanTaskId);
	if (taskIterator == m_taskMap.end())
		return;
	if (taskIterator->snapshot.state == CancelledFileTransferTaskState
		|| taskIterator->snapshot.state == CompletedFileTransferTaskState
		|| taskIterator->snapshot.state == FailedFileTransferTaskState)
	{
		return;
	}
	for (auto iterator = m_taskMap.begin(); iterator != m_taskMap.end(); ++iterator)
	{
		if (iterator->bPlanChild && iterator->strPlanTaskId == strPlanTaskId
			&& !requestWriteCancellation(&iterator.value()))
		{
			emit m_pOwner->transferError(QStringLiteral("commit_in_progress"),
				QStringLiteral("The current file is already being committed"));
			return;
		}
	}
	if (bNotifyRemote && m_bControlChannelOpen)
	{
		KFileTransferControlMessage cancel;
		cancel.type = CancelFileTransferControlMessageType;
		cancel.strTaskId = strPlanTaskId;
		cancel.strRequestId = taskIterator->strRequestId;
		sendControl(cancel);
	}
	QStringList childTaskIdList;
	for (auto iterator = m_taskMap.cbegin(); iterator != m_taskMap.cend(); ++iterator)
	{
		if (iterator->bPlanChild && iterator->strPlanTaskId == strPlanTaskId)
			childTaskIdList.append(iterator.key());
	}
	for (const QString &strChildTaskId : childTaskIdList)
	{
		auto childIterator = m_taskMap.find(strChildTaskId);
		if (childIterator == m_taskMap.end())
			continue;
		cleanupTaskResources(&childIterator.value());
		if (childIterator->snapshot.state != CompletedFileTransferTaskState)
			childIterator->snapshot.state = CancelledFileTransferTaskState;
		childIterator->snapshot.bCanPause = false;
		emitTask(childIterator.value());
	}
	if (planIterator != m_planMap.end())
	{
		cleanupPlanDirectories(strPlanTaskId);
		m_planTaskIdByRequestId.remove(planIterator->strRequestId);
		m_planMap.erase(planIterator);
	}
	taskIterator = m_taskMap.find(strPlanTaskId);
	if (taskIterator != m_taskMap.end())
	{
		taskIterator->snapshot.state = CancelledFileTransferTaskState;
		taskIterator->snapshot.bCanPause = false;
		taskIterator->snapshot.bCanRetry = true;
		emitTask(taskIterator.value());
	}
	if (m_strActiveTaskId == strPlanTaskId)
		m_strActiveTaskId.clear();
	startNextTask();
	emitActivity();
}

void KFileTransferSessionServicePrivate::cleanupPlanDirectories(
	const QString &strPlanTaskId)
{
	const auto iterator = m_planMap.constFind(strPlanTaskId);
	if (iterator == m_planMap.cend()
		|| iterator->createdDirectoryCleanupTokenList.isEmpty())
	{
		return;
	}
	const QStringList cleanupTokenList = iterator->createdDirectoryCleanupTokenList;
	queueFileSystemJob<KDirectoryCleanupJobResult>(
		[cleanupTokenList](IKFileSystemPort *pFileSystem)
		{
			KDirectoryCleanupJobResult result;
			result.bSuccess = pFileSystem->cleanupCreatedDirectories(
				cleanupTokenList, &result.result, &result.strError);
			return result;
		},
		[this, strPlanTaskId,
			nGeneration = m_nGeneration](KDirectoryCleanupJobResult result)
		{
			if (nGeneration == m_nGeneration && !result.bSuccess)
			{
				writeTrace(QStringLiteral("file_transfer_directory_cleanup_failed"),
					QStringLiteral("taskId=%1").arg(strPlanTaskId));
			}
		});
}

KFileTransferSessionServicePrivate::KCopyPlan *
KFileTransferSessionServicePrivate::planForRequest(const QString &strRequestId)
{
	const auto planIdIterator = m_planTaskIdByRequestId.constFind(strRequestId);
	if (planIdIterator == m_planTaskIdByRequestId.cend())
		return nullptr;
	auto planIterator = m_planMap.find(planIdIterator.value());
	return planIterator == m_planMap.end() ? nullptr : &planIterator.value();
}

void KFileTransferSessionServicePrivate::startSenderTask(const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	iterator->snapshot.state = ScanningFileTransferTaskState;
	emitTask(iterator.value());
	openSource(strTaskId);
}

void KFileTransferSessionServicePrivate::openSource(const QString &strTaskId)
{
	const auto iterator = m_taskMap.constFind(strTaskId);
	if (iterator == m_taskMap.cend())
		return;
	const QString strEntryId = iterator->strSourceEntryId;
	queueFileSystemJob<KSourceOpenResult>(
		[strEntryId](IKFileSystemPort *pFileSystem)
		{
			KSourceOpenResult result;
			result.bSuccess = pFileSystem->openSourceSnapshot(strEntryId,
				&result.snapshot, &result.strError);
			if (result.bSuccess)
			{
				result.spState = std::make_shared<KSourceWorkerState>();
				result.spState->strSourceId = result.snapshot.strSourceId;
			}
			return result;
		},
		[this, strTaskId, nGeneration = m_nGeneration](KSourceOpenResult result)
		{
			auto taskIterator = m_taskMap.find(strTaskId);
			if (nGeneration != m_nGeneration || taskIterator == m_taskMap.end()
				|| taskIterator->nGeneration != nGeneration
				|| taskIterator->snapshot.state != ScanningFileTransferTaskState)
			{
				if (result.bSuccess)
				{
					const QString strSourceId = result.snapshot.strSourceId;
					queueFileSystemJob<bool>([strSourceId](IKFileSystemPort *pFileSystem)
						{
							QString strIgnored;
							return pFileSystem->closeSourceSnapshot(strSourceId, &strIgnored);
						}, [](bool) {});
				}
				return;
			}
			if (!result.bSuccess)
			{
				failTask(strTaskId, QStringLiteral("source_changed"),
					result.strError);
				return;
			}
			KTask &task = taskIterator.value();
			if (task.snapshot.nBytesTotal != result.snapshot.nSize
				|| task.strFileName != result.snapshot.strName
				|| (task.lastModifiedUtc.isValid()
					&& result.snapshot.lastModifiedUtc.isValid()
					&& task.lastModifiedUtc != result.snapshot.lastModifiedUtc))
			{
				const QString strSourceId = result.snapshot.strSourceId;
				queueFileSystemJob<bool>([strSourceId](IKFileSystemPort *pFileSystem)
					{
						QString strIgnored;
						return pFileSystem->closeSourceSnapshot(strSourceId, &strIgnored);
					}, [](bool) {});
				failTask(strTaskId, QStringLiteral("source_changed"),
					QStringLiteral("The source file changed after it was listed"));
				return;
			}
			task.spSourceState = result.spState;
			task.strFileName = result.snapshot.strName;
			if (!task.bPlanChild)
				task.strRelativePath = result.snapshot.strName;
			task.lastModifiedUtc = result.snapshot.lastModifiedUtc;
			task.snapshot.strDisplayName = result.snapshot.strName;
			task.snapshot.nBytesTotal = result.snapshot.nSize;
			task.snapshot.nBytesTransferred = 0;
			task.snapshot.state = TransferringFileTransferTaskState;
			emitTask(task);

			KFileTransferControlMessage begin;
			begin.type = FileBeginFileTransferControlMessageType;
			begin.strRequestId = task.strRequestId;
			begin.strTaskId = task.snapshot.strTaskId;
			begin.strFileId = task.snapshot.strFileId;
			begin.strFileName = task.strFileName;
			begin.strRelativePath = task.strRelativePath;
			begin.strDestinationListingId = task.strDestinationListingId;
			begin.nSize = result.snapshot.nSize;
			begin.nModifiedAtMs = result.snapshot.lastModifiedUtc.isValid()
				? result.snapshot.lastModifiedUtc.toMSecsSinceEpoch() : 0;
			if (!sendControl(begin))
			{
				failTask(strTaskId, QStringLiteral("file_begin_failed"),
					QStringLiteral("Unable to send source file metadata"), false);
			}
		});
}

void KFileTransferSessionServicePrivate::pumpSender(const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	if (!task.bSender || task.snapshot.state != TransferringFileTransferTaskState
		|| task.bReadPending || task.bCompletionSent
		|| m_sessionState == ReconnectingSessionState
		|| m_pSessionController->isFileTransferBackpressured()
		|| task.nSentOffset - task.nAcknowledgedOffset >= kMaximumUnacknowledgedBytes
		|| task.spSourceState == nullptr)
	{
		return;
	}
	if (!task.pendingReadData.isEmpty() || task.bPendingReadComplete)
	{
		sendPendingSourceRead(&task);
		return;
	}

	task.bReadPending = true;
	const quint64 nOffset = task.nSentOffset;
	const std::shared_ptr<KSourceWorkerState> spSourceState = task.spSourceState;
	queueFileSystemJob<KSourceReadResult>(
		[spSourceState, nOffset](IKFileSystemPort *pFileSystem)
		{
			KSourceReadResult result;
			result.bSuccess = pFileSystem->readSourceChunk(
				spSourceState->strSourceId, nOffset, kDataChunkBytes,
				&result.data, &result.bComplete, &result.strError);
			return result;
		},
		[this, strTaskId, nOffset,
			nGeneration = m_nGeneration](KSourceReadResult result)
		{
			auto taskIterator = m_taskMap.find(strTaskId);
			if (nGeneration != m_nGeneration || taskIterator == m_taskMap.end()
				|| taskIterator->nGeneration != nGeneration
				|| (taskIterator->snapshot.state
					!= TransferringFileTransferTaskState
					&& taskIterator->snapshot.state
						!= PausedFileTransferTaskState))
				return;
			KTask &current = taskIterator.value();
			current.bReadPending = false;
			if (!result.bSuccess)
			{
				if (m_sessionState == ReconnectingSessionState)
					return;
				failTask(strTaskId, QStringLiteral("source_changed"), result.strError);
				return;
			}
			current.pendingReadData = std::move(result.data);
			current.nPendingReadOffset = nOffset;
			current.bPendingReadComplete = result.bComplete;
			if (m_sessionState != ReconnectingSessionState)
				sendPendingSourceRead(&current);
		});
}

bool KFileTransferSessionServicePrivate::sendPendingSourceRead(KTask *pTask)
{
	if (pTask == nullptr || pTask->spSourceState == nullptr
		|| (pTask->pendingReadData.isEmpty() && !pTask->bPendingReadComplete)
		|| m_sessionState == ReconnectingSessionState
		|| m_state != ReadyFileTransferState || !m_bDataChannelOpen)
	{
		return false;
	}
	if (pTask->nPendingReadOffset != pTask->nSentOffset)
	{
		failTask(pTask->snapshot.strTaskId, QStringLiteral("invalid_source_offset"),
			QStringLiteral("A pending source chunk no longer matches the task offset"));
		return false;
	}
	if (!pTask->pendingReadData.isEmpty())
	{
		KFileTransferDataFrame frame;
		frame.strTaskId = pTask->snapshot.strTaskId;
		frame.strFileId = pTask->snapshot.strFileId;
		frame.nOffset = pTask->nPendingReadOffset;
		frame.payload = pTask->pendingReadData;
		frame.nFlags = pTask->bPendingReadComplete ? kEndOfFileDataFrameFlag : 0;
		QString strEncodeError;
		const QByteArray encoded = KFileTransferDataFrameCodec::encode(
			frame, &strEncodeError);
		if (encoded.isEmpty())
		{
			failTask(pTask->snapshot.strTaskId, QStringLiteral("data_encode_failed"),
				strEncodeError);
			return false;
		}
		if (!m_pSessionController->sendFileTransferData(encoded))
		{
			if (m_sessionState == ReconnectingSessionState || !m_bDataChannelOpen
				|| m_pSessionController->isFileTransferBackpressured())
			{
				return false;
			}
			failTask(pTask->snapshot.strTaskId, QStringLiteral("data_send_failed"),
				QStringLiteral("Unable to send file data"));
			return false;
		}
		pTask->spSourceState->hash.addData(pTask->pendingReadData);
		pTask->nSentOffset += static_cast<quint64>(pTask->pendingReadData.size());
		pTask->snapshot.nBytesTransferred = pTask->nSentOffset;
		emitProgress(pTask,
			pTask->nSentOffset == pTask->snapshot.nBytesTotal);
	}
	const bool bComplete = pTask->bPendingReadComplete;
	pTask->pendingReadData.clear();
	pTask->bPendingReadComplete = false;
	pTask->nPendingReadOffset = 0;
	if (bComplete)
	{
		sendFileComplete(pTask, pTask->spSourceState->hash.result());
		return true;
	}
	pumpSender(pTask->snapshot.strTaskId);
	return true;
}

void KFileTransferSessionServicePrivate::sendFileComplete(
	KTask *pTask,
	const QByteArray &sha256)
{
	if (pTask == nullptr || pTask->bCompletionSent)
		return;
	KFileTransferControlMessage complete;
	complete.type = FileCompleteFileTransferControlMessageType;
	complete.strTaskId = pTask->snapshot.strTaskId;
	complete.strFileId = pTask->snapshot.strFileId;
	complete.nSize = pTask->snapshot.nBytesTotal;
	complete.sha256 = sha256;
	if (!sendControl(complete))
	{
		failTask(pTask->snapshot.strTaskId, QStringLiteral("complete_send_failed"),
			QStringLiteral("Unable to send file completion metadata"), false);
		return;
	}
	pTask->bCompletionSent = true;
}

void KFileTransferSessionServicePrivate::prepareReceiver(
	const KFileTransferControlMessage &message)
{
	auto existingIterator = m_taskMap.find(message.strTaskId);
	if (existingIterator == m_taskMap.end()
		|| existingIterator->bSender
		|| !existingIterator->snapshot.strFileId.isEmpty()
		|| existingIterator->strRequestId != message.strRequestId
		|| existingIterator->strDestinationListingId
			!= message.strDestinationListingId
		|| existingIterator->snapshot.state != ScanningFileTransferTaskState)
	{
		KFileTransferControlMessage error;
		error.type = ErrorFileTransferControlMessageType;
		error.strRequestId = message.strRequestId;
		error.strTaskId = message.strTaskId;
		error.strFileId = message.strFileId;
		error.strErrorCode = QStringLiteral("unknown_or_replayed_task");
		sendControl(error);
		return;
	}
	const KFileTransferDirection expectedDirection =
		m_pSessionController->sessionRole() == ControllerSessionRole
			? DownloadFileTransferDirection : UploadFileTransferDirection;
	if (existingIterator->snapshot.direction != expectedDirection)
	{
		failTask(message.strTaskId, QStringLiteral("task_direction_mismatch"),
			QStringLiteral("File metadata arrived for the wrong transfer direction"));
		return;
	}
	if (m_pSessionController->sessionRole() == ControllerSessionRole
		&& (existingIterator->strFileName != message.strFileName
			|| existingIterator->snapshot.nBytesTotal != message.nSize
			|| existingIterator->strRelativePath != message.strRelativePath
			|| (existingIterator->lastModifiedUtc.isValid()
				&& QDateTime::fromMSecsSinceEpoch(message.nModifiedAtMs,
					QTimeZone::UTC) != existingIterator->lastModifiedUtc)))
	{
		failTask(message.strTaskId, QStringLiteral("source_changed"),
			QStringLiteral("The remote source no longer matches its selected listing"));
		return;
	}
	const KOwnListing *pDestinationListing = ownListing(
		message.strDestinationListingId);
	const bool bPreparedPlanDirectory = existingIterator->bPlanChild
		&& !existingIterator->strDestinationDirectoryId.isEmpty();
	if (!bPreparedPlanDirectory
		&& (pDestinationListing == nullptr
			|| pDestinationListing->strDirectoryId.isEmpty()))
	{
		KFileTransferControlMessage error;
		error.type = ErrorFileTransferControlMessageType;
		error.strTaskId = message.strTaskId;
		error.strErrorCode = QStringLiteral("stale_destination");
		sendControl(error);
		return;
	}

	KTask task = existingIterator.value();
	task.snapshot.strTaskId = message.strTaskId;
	task.snapshot.strFileId = message.strFileId;
	task.snapshot.strDisplayName = message.strFileName;
	task.snapshot.nBytesTotal = message.nSize;
	task.snapshot.nBytesTransferred = 0;
	task.snapshot.state = ScanningFileTransferTaskState;
	task.snapshot.bCanPause = true;
	if (task.snapshot.direction == InvalidFileTransferDirection)
	{
		task.snapshot.direction = m_pSessionController->sessionRole()
			== ControllerSessionRole
			? DownloadFileTransferDirection : UploadFileTransferDirection;
	}
	task.strDestinationListingId = message.strDestinationListingId;
	if (!bPreparedPlanDirectory)
		task.strDestinationDirectoryId = pDestinationListing->strDirectoryId;
	task.strFileName = message.strFileName;
	task.strRelativePath = message.strRelativePath;
	task.lastModifiedUtc = QDateTime::fromMSecsSinceEpoch(
		message.nModifiedAtMs, QTimeZone::UTC);
	task.bSender = false;
	task.nQueuedReceiveOffset = 0;
	task.nWrittenOffset = 0;
	task.nLastAcknowledgedWriteOffset = 0;
	m_taskMap.insert(message.strTaskId, task);
	if (m_strActiveTaskId.isEmpty())
		m_strActiveTaskId = message.strTaskId;
	emitTask(m_taskMap.value(message.strTaskId));

	const QString strTaskId = message.strTaskId;
	const QString strDirectoryId = task.strDestinationDirectoryId;
	const QString strFileName = task.strFileName;
	queueFileSystemJob<KDestinationCheckResult>(
		[strDirectoryId, strFileName](IKFileSystemPort *pFileSystem)
		{
			KDestinationCheckResult result;
			result.bSuccess = pFileSystem->destinationExists(strDirectoryId,
				strFileName, &result.bExists, &result.strError);
			return result;
		},
		[this, strTaskId,
			nGeneration = m_nGeneration](KDestinationCheckResult result)
		{
			auto iterator = m_taskMap.find(strTaskId);
			if (nGeneration != m_nGeneration || iterator == m_taskMap.end()
				|| iterator->nGeneration != nGeneration
				|| iterator->snapshot.state != ScanningFileTransferTaskState)
				return;
			if (!result.bSuccess)
			{
				failTask(strTaskId, QStringLiteral("destination_unavailable"),
					result.strError);
				return;
			}
			if (!result.bExists)
			{
				openDestination(strTaskId, RejectExistingFileCollisionPolicy);
				return;
			}

			KTask &current = iterator.value();
			KFileTransferConflictResolution defaultResolution =
				m_defaultConflictResolution;
			if (current.bPlanChild)
			{
				const auto planIterator = m_planMap.constFind(current.strPlanTaskId);
				if (planIterator != m_planMap.cend())
					defaultResolution = planIterator->defaultConflictResolution;
			}
			if (defaultResolution != InvalidFileTransferConflictResolution)
			{
				if (defaultResolution == SkipFileTransferConflictResolution)
					finishTask(strTaskId, SkippedFileTransferTaskResult);
				else
				{
					openDestination(strTaskId,
						defaultResolution == OverwriteFileTransferConflictResolution
							? OverwriteFileCollisionPolicy : KeepBothFileCollisionPolicy);
				}
				return;
			}

			current.snapshot.state = WaitingConflictFileTransferTaskState;
			current.strConflictId = NewUuid();
			emitTask(current);
			if (m_pSessionController->sessionRole() == ControllerSessionRole)
			{
				KFileTransferConflictSnapshot conflict;
				conflict.strConflictId = current.strConflictId;
				conflict.strTaskId = current.snapshot.strTaskId;
				conflict.strFileId = current.snapshot.strFileId;
				conflict.strName = current.strFileName;
				conflict.nSourceSize = current.snapshot.nBytesTotal;
				conflict.sourceLastModifiedUtc = current.lastModifiedUtc;
				conflict.bApplyToRemainingAllowed = true;
				emit m_pOwner->conflictRequested(conflict);
			}
			else
			{
				KFileTransferControlMessage conflict;
				conflict.type = ConflictFileTransferControlMessageType;
				conflict.strTaskId = current.snapshot.strTaskId;
				conflict.strFileId = current.snapshot.strFileId;
				conflict.strConflictId = current.strConflictId;
				conflict.strFileName = current.strFileName;
				sendControl(conflict);
			}
		});
}

void KFileTransferSessionServicePrivate::openDestination(
	const QString &strTaskId,
	KFileCollisionPolicy policy)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	task.snapshot.state = TransferringFileTransferTaskState;
	task.strConflictId.clear();
	emitTask(task);
	KFileWriteRequest request;
	request.strDirectoryOpaqueId = task.strDestinationDirectoryId;
	request.strFileName = task.strFileName;
	request.nExpectedSize = task.snapshot.nBytesTotal;
	request.lastModifiedUtc = task.lastModifiedUtc;
	request.collisionPolicy = policy;
	queueFileSystemJob<KWriteOpenResult>(
		[request](IKFileSystemPort *pFileSystem)
		{
			KWriteOpenResult result;
			result.bSuccess = pFileSystem->beginWrite(request,
				&result.session, &result.strError);
			return result;
		},
		[this, strTaskId,
			nGeneration = m_nGeneration](KWriteOpenResult result)
		{
			auto taskIterator = m_taskMap.find(strTaskId);
			if (nGeneration != m_nGeneration || taskIterator == m_taskMap.end()
				|| taskIterator->nGeneration != nGeneration
				|| taskIterator->snapshot.state
					!= TransferringFileTransferTaskState)
			{
				if (result.bSuccess)
				{
					const QString strWriteId = result.session.strWriteId;
					queueFileSystemJob<bool>([strWriteId](IKFileSystemPort *pFileSystem)
						{
							QString strIgnored;
							return pFileSystem->abortWrite(strWriteId, &strIgnored);
						}, [](bool) {});
				}
				return;
			}
			if (!result.bSuccess)
			{
				failTask(strTaskId, QStringLiteral("destination_open_failed"),
					result.strError);
				return;
			}
			KTask &current = taskIterator.value();
			current.strWriteId = result.session.strWriteId;
			current.bWriteReady = true;
			KFileTransferControlMessage acknowledgement;
			acknowledgement.type = AckFileTransferControlMessageType;
			acknowledgement.strTaskId = current.snapshot.strTaskId;
			acknowledgement.strFileId = current.snapshot.strFileId;
			acknowledgement.nOffset = 0;
			sendControl(acknowledgement);
			tryFinalizeReceiver(strTaskId);
		});
}

void KFileTransferSessionServicePrivate::appendReceivedData(
	KTask *pTask,
	const KFileTransferDataFrame &frame)
{
	if (pTask == nullptr || !pTask->bWriteReady
		|| frame.nOffset != pTask->nQueuedReceiveOffset
		|| pTask->nQueuedReceiveOffset < pTask->nWrittenOffset
		|| static_cast<quint64>(frame.payload.size())
			> kMaximumUnacknowledgedBytes
		|| pTask->nQueuedReceiveOffset - pTask->nWrittenOffset
			> kMaximumUnacknowledgedBytes
				- static_cast<quint64>(frame.payload.size())
		|| frame.nOffset + static_cast<quint64>(frame.payload.size())
			> pTask->snapshot.nBytesTotal)
	{
		if (pTask != nullptr)
		{
			failTask(pTask->snapshot.strTaskId, QStringLiteral("invalid_offset"),
				QStringLiteral("File data arrived with an invalid offset"));
		}
		return;
	}
	pTask->nQueuedReceiveOffset += static_cast<quint64>(frame.payload.size());
	const QString strTaskId = pTask->snapshot.strTaskId;
	const QString strWriteId = pTask->strWriteId;
	const QByteArray data = frame.payload;
	queueFileSystemJob<KWriteResult>(
		[strWriteId, data](IKFileSystemPort *pFileSystem)
		{
			KWriteResult result;
			result.bSuccess = pFileSystem->appendWriteChunk(strWriteId,
				data, &result.strError);
			return result;
		},
		[this, strTaskId, nBytes = static_cast<quint64>(data.size()),
			nGeneration = m_nGeneration](KWriteResult result)
		{
			auto iterator = m_taskMap.find(strTaskId);
			if (nGeneration != m_nGeneration || iterator == m_taskMap.end()
				|| iterator->nGeneration != nGeneration
				|| (iterator->snapshot.state != TransferringFileTransferTaskState
					&& iterator->snapshot.state != PausedFileTransferTaskState))
				return;
			if (!result.bSuccess)
			{
				failTask(strTaskId, QStringLiteral("write_failed"), result.strError);
				return;
			}
			KTask &current = iterator.value();
			current.nWrittenOffset += nBytes;
			current.snapshot.nBytesTransferred = current.nWrittenOffset;
			emitProgress(&current,
				current.nWrittenOffset == current.snapshot.nBytesTotal);
			if (current.nWrittenOffset - current.nLastAcknowledgedWriteOffset
				>= kAcknowledgementIntervalBytes
				|| current.nWrittenOffset == current.snapshot.nBytesTotal)
			{
				KFileTransferControlMessage acknowledgement;
				acknowledgement.type = AckFileTransferControlMessageType;
				acknowledgement.strTaskId = current.snapshot.strTaskId;
				acknowledgement.strFileId = current.snapshot.strFileId;
				acknowledgement.nOffset = current.nWrittenOffset;
				if (sendControl(acknowledgement))
					current.nLastAcknowledgedWriteOffset = current.nWrittenOffset;
			}
			tryFinalizeReceiver(strTaskId);
		});
}

void KFileTransferSessionServicePrivate::tryFinalizeReceiver(
	const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	if (task.bSender || !task.bWriteReady || !task.bCompletionReceived
		|| task.bFinalizing || task.nWrittenOffset != task.snapshot.nBytesTotal)
	{
		return;
	}
	task.bFinalizing = true;
	const QString strWriteId = task.strWriteId;
	const QByteArray expectedSha256 = task.expectedSha256;
	queueFileSystemJob<KWriteResult>(
		[strWriteId, expectedSha256](IKFileSystemPort *pFileSystem)
		{
			KWriteResult result;
			result.bSuccess = pFileSystem->finalizeWrite(strWriteId,
				expectedSha256, &result.result, &result.strError);
			return result;
		},
		[this, strTaskId,
			nGeneration = m_nGeneration](KWriteResult result)
		{
			auto taskIterator = m_taskMap.find(strTaskId);
			if (nGeneration != m_nGeneration || taskIterator == m_taskMap.end()
				|| taskIterator->nGeneration != nGeneration
				|| !taskIterator->bFinalizing
				|| (taskIterator->snapshot.state
					!= TransferringFileTransferTaskState
					&& taskIterator->snapshot.state
						!= PausedFileTransferTaskState))
				return;
			if (!result.bSuccess)
			{
				failTask(strTaskId, QStringLiteral("integrity_check_failed"),
					result.strError);
				finishPendingStopIfReady();
				return;
			}
			taskIterator->strWriteId.clear();
			taskIterator->bWriteReady = false;
			finishTask(strTaskId, CompletedFileTransferTaskResult);
			finishPendingStopIfReady();
		});
}

void KFileTransferSessionServicePrivate::handleData(const QByteArray &data)
{
	if (m_state != ReadyFileTransferState
		&& m_state != ReconnectingFileTransferState)
	{
		return;
	}
	KFileTransferDataFrame frame;
	QString strError;
	if (!KFileTransferDataFrameCodec::decode(data, &frame, &strError))
	{
		emit m_pOwner->transferError(QStringLiteral("invalid_data_frame"), strError);
		return;
	}
	auto iterator = m_taskMap.find(frame.strTaskId);
	if (iterator == m_taskMap.end() || iterator->snapshot.strFileId != frame.strFileId
		|| iterator->bSender)
	{
		return;
	}
	appendReceivedData(&iterator.value(), frame);
}

void KFileTransferSessionServicePrivate::handleControlMessage(
	const KFileTransferControlMessage &message)
{
	if (m_state != ReadyFileTransferState
		&& m_state != ReconnectingFileTransferState)
	{
		return;
	}
	if (m_nGeneration == 0
		|| m_nGeneration != m_pSessionController->sessionGeneration())
	{
		return;
	}
	if (message.type == ListRootsRequestFileTransferControlMessageType
		|| message.type == ListDirectoryRequestFileTransferControlMessageType)
	{
		if (m_pSessionController->sessionRole() != ControlledSessionRole)
		{
			KFileTransferControlMessage error;
			error.type = ErrorFileTransferControlMessageType;
			error.strRequestId = message.strRequestId;
			error.strErrorCode = QStringLiteral("role_not_allowed");
			sendControl(error);
			return;
		}
		if (!message.strNextPageToken.isEmpty())
		{
			const auto tokenIterator = m_pageTokenMap.find(message.strNextPageToken);
			if (tokenIterator == m_pageTokenMap.end())
			{
				KFileTransferControlMessage error;
				error.type = ErrorFileTransferControlMessageType;
				error.strRequestId = message.strRequestId;
				error.strErrorCode = QStringLiteral("stale_page_token");
				sendControl(error);
				return;
			}
			const QPair<QString, int> page = tokenIterator.value();
			m_pageTokenMap.erase(tokenIterator);
			const KOwnListing *pListing = ownListing(page.first);
			if (pListing == nullptr)
				return;
			sendListingPage(*pListing, pListing->bRoots
				? ListRootsResponseFileTransferControlMessageType
				: ListDirectoryResponseFileTransferControlMessageType,
				message.strRequestId, page.second);
			return;
		}
		if (message.type == ListRootsRequestFileTransferControlMessageType)
		{
			listLocalRoots(RemoteFileTransferPane, true, message.strRequestId);
			return;
		}
		if (!message.strDisplayPath.isEmpty())
		{
			listLocalPath(RemoteFileTransferPane, message.strDisplayPath,
				true, message.strRequestId);
			return;
		}
		const KFileListingEntry *pEntry = ownEntry(
			message.strListingId, message.strEntryId);
		if (pEntry == nullptr
			|| (pEntry->type != DirectoryFileListingEntryType
				&& pEntry->type != DriveFileListingEntryType))
		{
			KFileTransferControlMessage error;
			error.type = ErrorFileTransferControlMessageType;
			error.strRequestId = message.strRequestId;
			error.strErrorCode = QStringLiteral("stale_listing");
			sendControl(error);
			return;
		}
		listLocalDirectory(RemoteFileTransferPane, pEntry->strOpaqueId,
			ChildDisplayPath(ownListing(message.strListingId)->strDisplayPath,
				pEntry->strName), true, message.strRequestId);
		return;
	}
	if (message.type == ListRootsResponseFileTransferControlMessageType
		|| message.type == ListDirectoryResponseFileTransferControlMessageType)
	{
		handleListingResponse(message);
		return;
	}
	if (message.type == ErrorFileTransferControlMessageType
		&& message.strTaskId.isEmpty())
	{
		if (message.strRequestId == m_remotePane.strPendingRequestId)
		{
			m_remotePane.accumulatedEntries.clear();
			emit m_pOwner->transferError(
				message.strErrorCode.isEmpty() ? QStringLiteral("remote_list_error")
					: message.strErrorCode,
				QStringLiteral("The remote directory request failed"));
		}
		return;
	}
	if (message.type == CopyRequestFileTransferControlMessageType)
	{
		if (m_pSessionController->sessionRole() != ControlledSessionRole
			|| message.entryIdList.isEmpty()
			|| !m_strActiveTaskId.isEmpty()
			|| m_taskMap.contains(message.strTaskId))
		{
			KFileTransferControlMessage error;
			error.type = ErrorFileTransferControlMessageType;
			error.strTaskId = message.strTaskId;
			error.strErrorCode = QStringLiteral("invalid_copy_request");
			sendControl(error);
			return;
		}
		m_defaultConflictResolution = InvalidFileTransferConflictResolution;
		if (message.direction == UploadFileTransferDirection)
		{
			const KOwnListing *pDestination = ownListing(
				message.strDestinationListingId);
			if (pDestination == nullptr || pDestination->strDirectoryId.isEmpty())
			{
				KFileTransferControlMessage error;
				error.type = ErrorFileTransferControlMessageType;
				error.strRequestId = message.strRequestId;
				error.strTaskId = message.strTaskId;
				error.strErrorCode = QStringLiteral("stale_destination");
				sendControl(error);
				return;
			}
			KTask task;
			task.snapshot.strTaskId = message.strTaskId;
			task.snapshot.kind = message.entryIdList.size() > 1
				? BatchFileTransferTaskKind : FileFileTransferTaskKind;
			task.snapshot.direction = UploadFileTransferDirection;
			task.snapshot.state = ScanningFileTransferTaskState;
			task.snapshot.bCanPause = true;
			task.strRequestId = message.strRequestId;
			task.strDestinationListingId = message.strDestinationListingId;
			task.strDestinationDirectoryId = pDestination->strDirectoryId;
			task.sourceEntryIdList = message.entryIdList;
			task.bPlan = message.entryIdList.size() > 1;
			task.bSender = false;
			task.nGeneration = m_nGeneration;
			m_taskMap.insert(message.strTaskId, task);
			m_rootTaskIdSet.insert(message.strTaskId);
			m_strActiveTaskId = message.strTaskId;
			emitTask(m_taskMap.value(message.strTaskId));
			return;
		}
		if (message.direction != DownloadFileTransferDirection)
		{
			KFileTransferControlMessage error;
			error.type = ErrorFileTransferControlMessageType;
			error.strRequestId = message.strRequestId;
			error.strTaskId = message.strTaskId;
			error.strErrorCode = QStringLiteral("invalid_direction");
			sendControl(error);
			return;
		}
		QVector<const KFileListingEntry *> entryList;
		entryList.reserve(message.entryIdList.size());
		bool bRequiresPlan = message.entryIdList.size() > 1;
		for (const QString &strEntryId : message.entryIdList)
		{
			const KFileListingEntry *pEntry = ownEntry(
				message.strSourceListingId, strEntryId);
			if (pEntry == nullptr
				|| (pEntry->type != RegularFileListingEntryType
					&& pEntry->type != DirectoryFileListingEntryType))
			{
				KFileTransferControlMessage error;
				error.type = ErrorFileTransferControlMessageType;
				error.strRequestId = message.strRequestId;
				error.strTaskId = message.strTaskId;
				error.strErrorCode = QStringLiteral("stale_source");
				sendControl(error);
				return;
			}
			entryList.append(pEntry);
			bRequiresPlan = bRequiresPlan
				|| pEntry->type == DirectoryFileListingEntryType;
		}
		if (bRequiresPlan)
		{
			KTask task;
			task.snapshot.strTaskId = message.strTaskId;
			task.snapshot.strDisplayName = entryList.size() == 1
				? entryList.first()->strName
				: QStringLiteral("%1 items").arg(entryList.size());
			task.snapshot.kind = entryList.size() == 1
				? FolderFileTransferTaskKind : BatchFileTransferTaskKind;
			task.snapshot.direction = DownloadFileTransferDirection;
			task.snapshot.state = QueuedFileTransferTaskState;
			task.snapshot.bCanPause = true;
			task.strRequestId = message.strRequestId;
			task.strSourceListingId = message.strSourceListingId;
			task.sourceEntryIdList = message.entryIdList;
			task.strDestinationListingId = message.strDestinationListingId;
			task.bSender = true;
			task.bPlan = true;
			task.nGeneration = m_nGeneration;
			m_taskMap.insert(message.strTaskId, task);
			m_rootTaskIdSet.insert(message.strTaskId);
			m_strActiveTaskId = message.strTaskId;
			emitTask(m_taskMap.value(message.strTaskId));
			startPlanTask(message.strTaskId);
			return;
		}
		const KFileListingEntry *pEntry = entryList.first();
		KTask task;
		task.snapshot.strTaskId = message.strTaskId;
		task.snapshot.strFileId = NewUuid();
		task.snapshot.strDisplayName = pEntry->strName;
		task.snapshot.kind = FileFileTransferTaskKind;
		task.snapshot.direction = DownloadFileTransferDirection;
		task.snapshot.state = QueuedFileTransferTaskState;
		task.snapshot.nBytesTotal = pEntry->nSize;
		task.snapshot.bCanPause = true;
		task.strRequestId = message.strRequestId;
		task.strSourceListingId = message.strSourceListingId;
		task.strSourceEntryId = pEntry->strOpaqueId;
		task.strDestinationListingId = message.strDestinationListingId;
		task.strFileName = pEntry->strName;
		task.strRelativePath = pEntry->strName;
		task.lastModifiedUtc = pEntry->lastModifiedUtc;
		task.bSender = true;
		task.nGeneration = m_nGeneration;
		m_taskMap.insert(message.strTaskId, task);
		m_rootTaskIdSet.insert(message.strTaskId);
		m_strActiveTaskId = message.strTaskId;
		emitTask(m_taskMap.value(message.strTaskId));
		startSenderTask(message.strTaskId);
		return;
	}
	if (message.type == FileBeginFileTransferControlMessageType)
	{
		if (planForRequest(message.strRequestId) != nullptr)
			processPlanFileBegin(message);
		else
			prepareReceiver(message);
		return;
	}
	if (message.type == CopyPlanBeginFileTransferControlMessageType)
	{
		handlePlanBegin(message);
		return;
	}
	if (message.type == CopyPlanDirectoryFileTransferControlMessageType)
	{
		handlePlanDirectory(message);
		return;
	}
	if (message.type == CopyPlanEndFileTransferControlMessageType)
	{
		handlePlanEnd(message);
		return;
	}

	auto iterator = m_taskMap.find(message.strTaskId);
	if (iterator == m_taskMap.end()
		|| (!message.strFileId.isEmpty()
			&& iterator->snapshot.strFileId != message.strFileId))
	{
		return;
	}
	KTask &task = iterator.value();
	if (message.type == AckFileTransferControlMessageType)
	{
		if (!task.bSender || message.nOffset < task.nAcknowledgedOffset
			|| message.nOffset > task.nSentOffset)
		{
			failTask(task.snapshot.strTaskId, QStringLiteral("invalid_ack"),
				QStringLiteral("Remote acknowledgement offset is invalid"));
			return;
		}
		task.nAcknowledgedOffset = message.nOffset;
		pumpSender(task.snapshot.strTaskId);
	}
	else if (message.type == PauseFileTransferControlMessageType)
	{
		if (task.snapshot.state == TransferringFileTransferTaskState)
		{
			task.snapshot.state = PausedFileTransferTaskState;
			emitTask(task);
		}
	}
	else if (message.type == ResumeFileTransferControlMessageType)
	{
		if (task.snapshot.state == PausedFileTransferTaskState)
		{
			task.snapshot.state = TransferringFileTransferTaskState;
			emitTask(task);
			pumpSender(task.snapshot.strTaskId);
		}
	}
	else if (message.type == CancelFileTransferControlMessageType)
	{
		cancelTask(task.snapshot.strTaskId, false);
	}
	else if (message.type == ConflictFileTransferControlMessageType)
	{
		if (m_pSessionController->sessionRole() != ControllerSessionRole
			|| !task.bSender)
		{
			return;
		}
		task.strConflictId = message.strConflictId;
		task.snapshot.state = WaitingConflictFileTransferTaskState;
		emitTask(task);
		KFileTransferConflictSnapshot conflict;
		conflict.strConflictId = message.strConflictId;
		conflict.strTaskId = task.snapshot.strTaskId;
		conflict.strFileId = task.snapshot.strFileId;
		conflict.strName = message.strFileName;
		conflict.nSourceSize = task.snapshot.nBytesTotal;
		conflict.sourceLastModifiedUtc = task.lastModifiedUtc;
		conflict.bApplyToRemainingAllowed = true;
		emit m_pOwner->conflictRequested(conflict);
	}
	else if (message.type == ConflictResolutionFileTransferControlMessageType)
	{
		if (task.bSender || task.strConflictId != message.strConflictId)
			return;
		if (message.bApplyToRemaining)
		{
			if (task.bPlanChild)
			{
				auto planIterator = m_planMap.find(task.strPlanTaskId);
				if (planIterator != m_planMap.end())
					planIterator->defaultConflictResolution = message.conflictResolution;
			}
			else
				m_defaultConflictResolution = message.conflictResolution;
		}
		if (message.conflictResolution == SkipFileTransferConflictResolution)
			finishTask(task.snapshot.strTaskId, SkippedFileTransferTaskResult);
		else
		{
			openDestination(task.snapshot.strTaskId,
				message.conflictResolution == OverwriteFileTransferConflictResolution
					? OverwriteFileCollisionPolicy : KeepBothFileCollisionPolicy);
		}
	}
	else if (message.type == FileCompleteFileTransferControlMessageType)
	{
		if (task.bSender || message.nSize != task.snapshot.nBytesTotal
			|| message.sha256.size() != KFileTransferControlMessageCodec::kSha256Bytes)
		{
			failTask(task.snapshot.strTaskId, QStringLiteral("invalid_completion"),
				QStringLiteral("Remote completion metadata is invalid"));
			return;
		}
		task.expectedSha256 = message.sha256;
		task.bCompletionReceived = true;
		tryFinalizeReceiver(task.snapshot.strTaskId);
	}
	else if (message.type == TaskCompleteFileTransferControlMessageType)
	{
		if (!task.bSender)
			return;
		finishTask(task.snapshot.strTaskId, message.taskResult);
	}
	else if (message.type == ErrorFileTransferControlMessageType)
	{
		failTask(task.snapshot.strTaskId,
			message.strErrorCode.isEmpty() ? QStringLiteral("remote_error")
				: message.strErrorCode,
			QStringLiteral("The remote file transfer task failed"), false);
	}
}

void KFileTransferSessionServicePrivate::pauseTask(const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end()
		|| iterator->snapshot.state != TransferringFileTransferTaskState)
	{
		return;
	}
	if (iterator->bPlan)
	{
		iterator->snapshot.state = PausedFileTransferTaskState;
		iterator->bPausedForReconnect = false;
		emitTask(iterator.value());
		for (auto childIterator = m_taskMap.begin();
			childIterator != m_taskMap.end(); ++childIterator)
		{
			if (childIterator->bPlanChild
				&& childIterator->strPlanTaskId == strTaskId
				&& childIterator->snapshot.state
					== TransferringFileTransferTaskState)
			{
				pauseTask(childIterator.key());
				break;
			}
		}
		return;
	}
	iterator->snapshot.state = PausedFileTransferTaskState;
	iterator->bPausedForReconnect = false;
	emitTask(iterator.value());
	KFileTransferControlMessage message;
	message.type = PauseFileTransferControlMessageType;
	message.strTaskId = iterator->snapshot.strTaskId;
	message.strFileId = iterator->snapshot.strFileId;
	sendControl(message);
}

void KFileTransferSessionServicePrivate::resumeTask(const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end()
		|| iterator->snapshot.state != PausedFileTransferTaskState
		|| m_sessionState == ReconnectingSessionState)
	{
		return;
	}
	if (iterator->bPlan)
	{
		iterator->snapshot.state = TransferringFileTransferTaskState;
		iterator->bPausedForReconnect = false;
		emitTask(iterator.value());
		for (auto childIterator = m_taskMap.begin();
			childIterator != m_taskMap.end(); ++childIterator)
		{
			if (childIterator->bPlanChild
				&& childIterator->strPlanTaskId == strTaskId
				&& childIterator->snapshot.state == PausedFileTransferTaskState)
			{
				resumeTask(childIterator.key());
				break;
			}
		}
		return;
	}
	iterator->snapshot.state = TransferringFileTransferTaskState;
	iterator->bPausedForReconnect = false;
	emitTask(iterator.value());
	KFileTransferControlMessage message;
	message.type = ResumeFileTransferControlMessageType;
	message.strTaskId = iterator->snapshot.strTaskId;
	message.strFileId = iterator->snapshot.strFileId;
	sendControl(message);
	pumpSender(strTaskId);
}

void KFileTransferSessionServicePrivate::cancelTask(
	const QString &strTaskId,
	bool bNotifyRemote)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	if (task.bPlan)
	{
		cancelPlan(strTaskId, bNotifyRemote);
		return;
	}
	if (task.bPlanChild)
	{
		cancelPlan(task.strPlanTaskId, bNotifyRemote);
		return;
	}
	if (!requestWriteCancellation(&task))
	{
		emit m_pOwner->transferError(QStringLiteral("commit_in_progress"),
			QStringLiteral("The file is already being committed"));
		return;
	}
	if (task.snapshot.state == CompletedFileTransferTaskState
		|| task.snapshot.state == CancelledFileTransferTaskState)
	{
		return;
	}
	if (bNotifyRemote && m_bControlChannelOpen)
	{
		KFileTransferControlMessage message;
		message.type = CancelFileTransferControlMessageType;
		message.strTaskId = task.snapshot.strTaskId;
		message.strFileId = task.snapshot.strFileId;
		sendControl(message);
	}
	task.snapshot.state = CancelledFileTransferTaskState;
	task.snapshot.bCanPause = false;
	task.snapshot.bCanRetry = true;
	cleanupTaskResources(&task);
	emitTask(task);
	if (m_strActiveTaskId == strTaskId)
	{
		m_strActiveTaskId.clear();
		startNextTask();
	}
	emitActivity();
}

void KFileTransferSessionServicePrivate::retryTask(const QString &strTaskId)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end()
		|| (iterator->snapshot.state != FailedFileTransferTaskState
			&& iterator->snapshot.state != CancelledFileTransferTaskState))
	{
		return;
	}
	if (iterator->bPlanChild)
	{
		retryTask(iterator->strPlanTaskId);
		return;
	}
	KTask &task = iterator.value();
	task.snapshot.strFileId = NewUuid();
	task.snapshot.state = QueuedFileTransferTaskState;
	task.snapshot.nBytesTransferred = 0;
	task.snapshot.strErrorCode.clear();
	task.snapshot.bCanPause = true;
	task.snapshot.bCanRetry = false;
	task.strRequestId = NewUuid();
	task.strWriteId.clear();
	task.strConflictId.clear();
	task.expectedSha256.clear();
	task.spSourceState.reset();
	task.nSentOffset = 0;
	task.nAcknowledgedOffset = 0;
	task.nQueuedReceiveOffset = 0;
	task.nWrittenOffset = 0;
	task.nLastAcknowledgedWriteOffset = 0;
	task.nLastProgressPublishedMs = 0;
	task.bReadPending = false;
	task.bWriteReady = false;
	task.bCompletionReceived = false;
	task.bCompletionSent = false;
	task.bFinalizing = false;
	task.bPausedForReconnect = false;
	m_taskQueue.enqueue(strTaskId);
	emitTask(task);
	startNextTask();
}

void KFileTransferSessionServicePrivate::resolveConflict(
	const QString &strConflictId,
	KFileTransferConflictResolution resolution,
	bool bApplyToRemaining)
{
	if (resolution != OverwriteFileTransferConflictResolution
		&& resolution != SkipFileTransferConflictResolution
		&& resolution != KeepBothFileTransferConflictResolution)
	{
		return;
	}
	auto iterator = std::find_if(m_taskMap.begin(), m_taskMap.end(),
		[&strConflictId](const KTask &task)
		{
			return task.strConflictId == strConflictId
				&& task.snapshot.state == WaitingConflictFileTransferTaskState;
		});
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	if (bApplyToRemaining)
	{
		if (task.bPlanChild)
		{
			auto planIterator = m_planMap.find(task.strPlanTaskId);
			if (planIterator != m_planMap.end())
				planIterator->defaultConflictResolution = resolution;
		}
		else
			m_defaultConflictResolution = resolution;
	}
	if (task.bSender)
	{
		KFileTransferControlMessage message;
		message.type = ConflictResolutionFileTransferControlMessageType;
		message.strTaskId = task.snapshot.strTaskId;
		message.strFileId = task.snapshot.strFileId;
		message.strConflictId = task.strConflictId;
		message.conflictResolution = resolution;
		message.bApplyToRemaining = bApplyToRemaining;
		if (sendControl(message))
		{
			task.snapshot.state = TransferringFileTransferTaskState;
			task.strConflictId.clear();
			emitTask(task);
		}
		return;
	}
	if (resolution == SkipFileTransferConflictResolution)
		finishTask(task.snapshot.strTaskId, SkippedFileTransferTaskResult);
	else
	{
		openDestination(task.snapshot.strTaskId,
			resolution == OverwriteFileTransferConflictResolution
				? OverwriteFileCollisionPolicy : KeepBothFileCollisionPolicy);
	}
}

void KFileTransferSessionServicePrivate::clearCompletedTasks()
{
	QStringList removeIds;
	for (auto iterator = m_taskMap.cbegin(); iterator != m_taskMap.cend(); ++iterator)
	{
		if (iterator->snapshot.state == CompletedFileTransferTaskState
			|| iterator->snapshot.state == FailedFileTransferTaskState
			|| iterator->snapshot.state == CancelledFileTransferTaskState)
		{
			removeIds.append(iterator.key());
		}
	}
	for (const QString &strTaskId : removeIds)
	{
		m_taskMap.remove(strTaskId);
		m_rootTaskIdSet.remove(strTaskId);
		emit m_pOwner->taskRemoved(strTaskId);
	}
	requestSnapshot();
}

void KFileTransferSessionServicePrivate::finishTask(
	const QString &strTaskId,
	KFileTransferTaskResult result)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	if (task.snapshot.state == CompletedFileTransferTaskState
		|| task.snapshot.state == CancelledFileTransferTaskState
		|| task.snapshot.state == FailedFileTransferTaskState)
	{
		return;
	}
	const bool bPlanChild = task.bPlanChild;
	const QString strPlanTaskId = task.strPlanTaskId;
	if (!task.bSender)
	{
		KFileTransferControlMessage complete;
		complete.type = TaskCompleteFileTransferControlMessageType;
		complete.strTaskId = task.snapshot.strTaskId;
		complete.strFileId = task.snapshot.strFileId;
		complete.taskResult = result;
		sendControl(complete);
	}
	task.snapshot.state = result == SkippedFileTransferTaskResult
		? CancelledFileTransferTaskState : CompletedFileTransferTaskState;
	task.snapshot.nBytesTransferred = result == CompletedFileTransferTaskResult
		? task.snapshot.nBytesTotal : task.snapshot.nBytesTransferred;
	task.snapshot.bCanPause = false;
	task.snapshot.bCanRetry = result == SkippedFileTransferTaskResult;
	cleanupTaskResources(&task);
	emitTask(task);
	writeTrace(QStringLiteral("file_transfer_task_finished"),
		QStringLiteral("taskId=%1 result=%2 bytes=%3")
			.arg(strTaskId,
				KFileTransferControlMessageCodec::taskResultName(result))
			.arg(task.snapshot.nBytesTransferred));
	if (bPlanChild)
	{
		auto planIterator = m_planMap.find(strPlanTaskId);
		auto planTaskIterator = m_taskMap.find(strPlanTaskId);
		if (planIterator != m_planMap.end()
			&& planTaskIterator != m_taskMap.end())
		{
			++planIterator->nCompletedFileCount;
			planIterator->bHadSkippedFile = planIterator->bHadSkippedFile
				|| result == SkippedFileTransferTaskResult;
			KTask &planTask = planTaskIterator.value();
			planTask.snapshot.nBytesTransferred = qMin(
				planTask.snapshot.nBytesTotal,
				planTask.snapshot.nBytesTransferred
					+ task.snapshot.nBytesTransferred);
			emitTask(planTask);
		}
	}
	if (m_strActiveTaskId == strTaskId)
	{
		m_strActiveTaskId.clear();
		startNextTask();
	}
	if (bPlanChild)
		completePlanIfReady(strPlanTaskId);
	emitActivity();
}

void KFileTransferSessionServicePrivate::failTask(
	const QString &strTaskId,
	const QString &strErrorCode,
	const QString &strTechnicalMessage,
	bool bNotifyRemote)
{
	auto iterator = m_taskMap.find(strTaskId);
	if (iterator == m_taskMap.end())
		return;
	KTask &task = iterator.value();
	if (task.snapshot.state == FailedFileTransferTaskState
		|| task.snapshot.state == CompletedFileTransferTaskState
		|| task.snapshot.state == CancelledFileTransferTaskState)
	{
		return;
	}
	const bool bPlan = task.bPlan;
	const bool bPlanChild = task.bPlanChild;
	const QString strPlanTaskId = task.strPlanTaskId;
	if (bNotifyRemote && m_bControlChannelOpen)
	{
		KFileTransferControlMessage error;
		error.type = ErrorFileTransferControlMessageType;
		error.strTaskId = task.snapshot.strTaskId;
		error.strFileId = task.snapshot.strFileId;
		error.strErrorCode = strErrorCode;
		sendControl(error);
	}
	task.snapshot.state = FailedFileTransferTaskState;
	task.snapshot.strErrorCode = strErrorCode;
	task.snapshot.bCanPause = false;
	task.snapshot.bCanRetry = true;
	cleanupTaskResources(&task);
	emitTask(task);
	emit m_pOwner->transferError(strErrorCode, strTechnicalMessage);
	writeTrace(QStringLiteral("file_transfer_task_failed"),
		QStringLiteral("taskId=%1 code=%2 bytes=%3")
			.arg(strTaskId, strErrorCode)
			.arg(task.snapshot.nBytesTransferred));
	if (bPlan)
	{
		QStringList childTaskIdList;
		for (auto childIterator = m_taskMap.cbegin();
			childIterator != m_taskMap.cend(); ++childIterator)
		{
			if (childIterator->bPlanChild
				&& childIterator->strPlanTaskId == strTaskId)
			{
				childTaskIdList.append(childIterator.key());
			}
		}
		for (const QString &strChildTaskId : childTaskIdList)
		{
			auto childIterator = m_taskMap.find(strChildTaskId);
			if (childIterator == m_taskMap.end())
				continue;
			cleanupTaskResources(&childIterator.value());
			if (childIterator->snapshot.state
				!= CompletedFileTransferTaskState)
			{
				childIterator->snapshot.state = FailedFileTransferTaskState;
				childIterator->snapshot.strErrorCode = strErrorCode;
				childIterator->snapshot.bCanPause = false;
				emitTask(childIterator.value());
			}
		}
		auto planIterator = m_planMap.find(strTaskId);
		if (planIterator != m_planMap.end())
		{
			cleanupPlanDirectories(strTaskId);
			m_planTaskIdByRequestId.remove(planIterator->strRequestId);
			m_planMap.erase(planIterator);
		}
	}
	if (m_strActiveTaskId == strTaskId)
	{
		m_strActiveTaskId.clear();
		startNextTask();
	}
	if (bPlanChild)
		failTask(strPlanTaskId, strErrorCode, strTechnicalMessage, false);
	emitActivity();
}

bool KFileTransferSessionServicePrivate::requestWriteCancellation(KTask *pTask)
{
	if (pTask == nullptr || pTask->strWriteId.isEmpty())
		return true;
	if (m_spFileSystemState->spFileSystem == nullptr)
		return false;
	return m_spFileSystemState->spFileSystem->tryRequestWriteCancellation(
		pTask->strWriteId);
}

void KFileTransferSessionServicePrivate::cleanupTaskResources(KTask *pTask)
{
	if (pTask == nullptr)
		return;
	if (pTask->spSourceState != nullptr)
	{
		const QString strSourceId = pTask->spSourceState->strSourceId;
		pTask->spSourceState.reset();
		queueFileSystemJob<bool>([strSourceId](IKFileSystemPort *pFileSystem)
			{
				QString strIgnored;
				return pFileSystem->closeSourceSnapshot(strSourceId, &strIgnored);
			}, [](bool) {});
	}
	if (!pTask->strWriteId.isEmpty())
	{
		const QString strWriteId = pTask->strWriteId;
		pTask->strWriteId.clear();
		pTask->bWriteReady = false;
		queueFileSystemJob<bool>([strWriteId](IKFileSystemPort *pFileSystem)
			{
				QString strIgnored;
				return pFileSystem->abortWrite(strWriteId, &strIgnored);
			}, [](bool) {});
	}
}

void KFileTransferSessionServicePrivate::emitTask(const KTask &task)
{
	if (task.bPlanChild)
		return;
	emit m_pOwner->taskChanged(task.snapshot);
	emitActivity();
}

void KFileTransferSessionServicePrivate::emitProgress(
	KTask *pTask,
	bool bForce)
{
	if (pTask == nullptr)
		return;
	const qint64 nNowMs = QDateTime::currentMSecsSinceEpoch();
	if (!bForce && pTask->nLastProgressPublishedMs > 0
		&& nNowMs - pTask->nLastProgressPublishedMs < kProgressPublishIntervalMs)
	{
		return;
	}
	pTask->nLastProgressPublishedMs = nNowMs;
	emitTask(*pTask);
}

void KFileTransferSessionServicePrivate::emitActivity()
{
	int nActiveTaskCount = 0;
	for (const QString &strTaskId : m_rootTaskIdSet)
	{
		const auto iterator = m_taskMap.constFind(strTaskId);
		if (iterator == m_taskMap.cend())
			continue;
		const KTask &task = iterator.value();
		if (task.snapshot.state == ScanningFileTransferTaskState
			|| task.snapshot.state == QueuedFileTransferTaskState
			|| task.snapshot.state == TransferringFileTransferTaskState
			|| task.snapshot.state == PausedFileTransferTaskState
			|| task.snapshot.state == WaitingConflictFileTransferTaskState)
		{
			++nActiveTaskCount;
		}
	}
	emit m_pOwner->controlledActivityChanged(
		m_pSessionController->sessionRole() == ControlledSessionRole
			&& m_state != ClosedFileTransferState,
		nActiveTaskCount);
}

bool KFileTransferSessionServicePrivate::sendControl(
	const KFileTransferControlMessage &message)
{
	if (!m_bControlChannelOpen)
		return false;
	if (!m_controlQueue.isEmpty()
		|| m_pSessionController->isFileTransferBackpressured())
	{
		return enqueueControl(message);
	}
	if (m_pSessionController->sendFileTransferControlMessage(message))
		return true;
	return enqueueControl(message);
}

bool KFileTransferSessionServicePrivate::enqueueControl(
	const KFileTransferControlMessage &message)
{
	const int nEncodedBytes = KFileTransferControlMessageCodec::encode(message)
		.toUtf8().size();
	if (nEncodedBytes <= 0)
		return false;
	if (message.type == AckFileTransferControlMessageType)
	{
		for (KQueuedControlMessage &queued : m_controlQueue)
		{
			if (queued.message.type == AckFileTransferControlMessageType
				&& queued.message.strTaskId == message.strTaskId
				&& queued.message.strFileId == message.strFileId)
			{
				const quint64 nOldOffset = queued.message.nOffset;
				queued.message.nOffset = qMax(nOldOffset, message.nOffset);
				const int nUpdatedBytes =
					KFileTransferControlMessageCodec::encode(queued.message)
						.toUtf8().size();
				if (nUpdatedBytes > kMaximumQueuedControlBytes
					- (m_nQueuedControlBytes - queued.nEncodedBytes))
				{
					queued.message.nOffset = nOldOffset;
					return true;
				}
				m_nQueuedControlBytes += nUpdatedBytes - queued.nEncodedBytes;
				queued.nEncodedBytes = nUpdatedBytes;
				return true;
			}
		}
	}
	if (m_controlQueue.size() >= kMaximumQueuedControlMessages
		|| nEncodedBytes > kMaximumQueuedControlBytes - m_nQueuedControlBytes)
	{
		emit m_pOwner->transferError(QStringLiteral("control_queue_overflow"),
			QStringLiteral("The file transfer control queue exceeded its limit"));
		return false;
	}
	KQueuedControlMessage queued;
	queued.message = message;
	queued.nEncodedBytes = nEncodedBytes;
	if (message.type == AckFileTransferControlMessageType)
		m_controlQueue.prepend(queued);
	else
		m_controlQueue.enqueue(queued);
	m_nQueuedControlBytes += nEncodedBytes;
	return true;
}

void KFileTransferSessionServicePrivate::flushControlQueue()
{
	while (m_bControlChannelOpen && !m_controlQueue.isEmpty()
		&& !m_pSessionController->isFileTransferBackpressured())
	{
		const KQueuedControlMessage &queued = m_controlQueue.head();
		if (!m_pSessionController->sendFileTransferControlMessage(queued.message))
			break;
		m_nQueuedControlBytes -= queued.nEncodedBytes;
		m_controlQueue.dequeue();
	}
	if (m_nQueuedControlBytes < 0)
		m_nQueuedControlBytes = 0;
}

bool KFileTransferSessionServicePrivate::sendLifecycle(
	KFileTransferLifecycleMessageType type,
	const QString &strErrorCode)
{
	KFileTransferLifecycleMessage message;
	message.type = type;
	message.strRequestId = m_strLifecycleRequestId;
	message.nGeneration = m_nGeneration;
	message.strErrorCode = strErrorCode;
	return m_pSessionController->sendFileTransferLifecycleMessage(message);
}

void KFileTransferSessionServicePrivate::workerJobFinished()
{
	if (m_nPendingWorkerJobs > 0)
		--m_nPendingWorkerJobs;
	finishShutdownIfReady();
}

void KFileTransferSessionServicePrivate::shutdown()
{
	if (m_bShutdownRequested)
		return;
	m_bShutdownRequested = true;
	stopCurrentFileTransfer(false);
	finishShutdownIfReady();
}

void KFileTransferSessionServicePrivate::finishShutdownIfReady()
{
	if (!m_bShutdownRequested || m_nPendingWorkerJobs != 0 || m_bWorkerStopping)
		return;
	m_bWorkerStopping = true;
	m_spFileSystemState->bClosed.store(true);
	if (m_pWorkerThread != nullptr && m_pWorkerThread->isRunning())
	{
		QObject::connect(m_pWorkerThread, &QThread::finished,
			m_pOwner, [this]()
			{
				if (m_bShutdownFinishedEmitted)
					return;
				m_bShutdownFinishedEmitted = true;
				emit m_pOwner->shutdownFinished();
			});
		m_pWorkerThread->quit();
		return;
	}
	if (!m_bShutdownFinishedEmitted)
	{
		m_bShutdownFinishedEmitted = true;
		emit m_pOwner->shutdownFinished();
	}
}
