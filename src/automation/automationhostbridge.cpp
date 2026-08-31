#include "automation/automationhostbridge.h"

#include "commands/applicationcommandregistry.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionerror.h"
#include "core/session/sessionstatemachine.h"
#include "core/discovery/discovereddevice.h"
#include "core/devices/recentdevice.h"
#include "core/file_transfer/filetransferstate.h"
#include "core/security/permissionscope.h"
#include "core/security/trusteddevice.h"
#include "session/sessioncontroller.h"
#include "session/sessioncoordinator.h"
#include "ui_bridge/devicediscoveryviewmodel.h"
#include "devices/recentdeviceservice.h"
#include "settings/applicationsettingsservice.h"
#include "clipboard/clipboardsyncservice.h"
#include "terminal/terminalsessionservice.h"
#include "file_transfer/filetransfersessionservice.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>

#include <cstring>

namespace
{
	constexpr int kMaximumAutomationEvents = 512;
	constexpr int kMaximumCriticalAutomationEvents = 256;
	constexpr int kMaximumStateAutomationEvents = 192;
	constexpr int kMaximumTelemetryAutomationEvents = 64;

	QByteArray BytesFromRaw(const char *pData, std::uint32_t nBytes)
	{
		if (pData == nullptr || nBytes == 0)
			return QByteArray();
		return QByteArray(pData, static_cast<qsizetype>(nBytes));
	}

	QString PrivacyModeName(KPrivacyMode mode)
	{
		if (mode == PrivacyOverlayPrivacyMode)
			return QStringLiteral("privacyOverlay");
		if (mode == DisplayOffPrivacyMode)
			return QStringLiteral("displayOff");
		return QStringLiteral("disabled");
	}

	QString PrivacyStateName(KPrivacyModeState state)
	{
		if (state == ApplyingPrivacyModeState)
			return QStringLiteral("applying");
		if (state == ActivePrivacyModeState)
			return QStringLiteral("active");
		if (state == RestoringPrivacyModeState)
			return QStringLiteral("restoring");
		if (state == FailedPrivacyModeState)
			return QStringLiteral("failed");
		return QStringLiteral("inactive");
	}

	QString PostSessionActionName(KPostSessionAction action)
	{
		return action == LockWorkstationPostSessionAction
			? QStringLiteral("lockWorkstation")
			: QStringLiteral("none");
	}

	QJsonObject ErrorObject(const QString &strErrorCode, const QString &strMessage)
	{
		return QJsonObject{
			{QStringLiteral("status"), static_cast<int>(ApplicationCommandFailed)},
			{QStringLiteral("value"), QJsonObject()},
			{QStringLiteral("errorCode"), strErrorCode},
			{QStringLiteral("technicalMessage"), strMessage}
		};
	}

	QString FilePaneName(KFileTransferPane pane)
	{
		return pane == RemoteFileTransferPane
			? QStringLiteral("remote") : QStringLiteral("local");
	}

	QString FileEntryKind(KFileListingEntryType type)
	{
		if (type == DriveFileListingEntryType)
			return QStringLiteral("drive");
		if (type == DirectoryFileListingEntryType)
			return QStringLiteral("directory");
		if (type == RegularFileListingEntryType)
			return QStringLiteral("file");
		return QStringLiteral("unknown");
	}

	QJsonObject FileTaskObject(const KFileTransferTaskSnapshot &task)
	{
		return QJsonObject{
			{QStringLiteral("taskId"), task.strTaskId},
			{QStringLiteral("fileId"), task.strFileId},
			{QStringLiteral("displayName"), task.strDisplayName},
			{QStringLiteral("kind"), FileTransferTaskKindName(task.kind)},
			{QStringLiteral("direction"), task.direction == DownloadFileTransferDirection
				? QStringLiteral("download") : QStringLiteral("upload")},
			{QStringLiteral("status"), FileTransferTaskStateName(task.state)},
			{QStringLiteral("bytesTransferred"), QString::number(task.nBytesTransferred)},
			{QStringLiteral("bytesTotal"), QString::number(task.nBytesTotal)},
			{QStringLiteral("errorCode"), task.strErrorCode},
			{QStringLiteral("canPause"), task.bCanPause},
			{QStringLiteral("canRetry"), task.bCanRetry}
		};
	}
}

KAutomationHostBridge::KAutomationHostBridge(KApplicationCommandRegistry *pRegistry,
	KSessionController *pSessionController,
	const QString &strDataDirectory,
	QObject *pParent)
	: QObject(pParent)
	, m_pRegistry(pRegistry)
	, m_pSessionController(pSessionController)
	, m_strDataDirectory(strDataDirectory)
	, m_nObservedSessionGeneration(pSessionController->sessionGeneration())
{
	Q_ASSERT(m_pRegistry != nullptr);
	Q_ASSERT(m_pSessionController != nullptr);
	m_hostApi.pHostContext = this;
	m_hostApi.submitCommand = &KAutomationHostBridge::SubmitCommand;
	m_hostApi.requestSnapshot = &KAutomationHostBridge::RequestSnapshot;
	m_hostApi.copyHostValue = &KAutomationHostBridge::CopyHostValue;
	m_hostApi.isHostReady = &KAutomationHostBridge::IsHostReady;
	m_hostApi.writeLog = &KAutomationHostBridge::WriteLog;
	initializeStateConnections();
}

KAutomationHostBridge::~KAutomationHostBridge()
{
	stopAcceptingRequests();
}

const KWrcDriverHostApiV2 *KAutomationHostBridge::hostApi() const
{
	return &m_hostApi;
}

void KAutomationHostBridge::observeApplicationFeatures(
	KSessionCoordinator *pSessionCoordinator,
	KDeviceDiscoveryViewModel *pDiscoveryViewModel,
	KRecentDeviceService *pRecentDeviceService,
	KApplicationSettingsService *pSettingsService,
	KClipboardSyncService *pClipboardService,
	KTerminalSessionService *pTerminalService,
	KFileTransferSessionService *pFileTransferService)
{
	Q_ASSERT(pSessionCoordinator != nullptr);
	Q_ASSERT(pDiscoveryViewModel != nullptr);
	Q_ASSERT(pRecentDeviceService != nullptr);
	Q_ASSERT(pSettingsService != nullptr);
	Q_ASSERT(pClipboardService != nullptr);
	Q_ASSERT(pTerminalService != nullptr);
	Q_ASSERT(pFileTransferService != nullptr);

	connect(pDiscoveryViewModel, &KDeviceDiscoveryViewModel::lanDevicesChanged,
		this, [this](const QVector<KDiscoveredDevice> &devices)
		{
			QJsonArray items;
			for (const KDiscoveredDevice &device : devices)
			{
				items.append(QJsonObject{
					{QStringLiteral("deviceId"), device.strDeviceId},
					{QStringLiteral("name"), device.strDeviceName},
					{QStringLiteral("address"), device.strHost},
					{QStringLiteral("port"), device.nSignalingPort},
					{QStringLiteral("online"), true}
				});
			}
			m_lanDevices = items;
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("discovery.devices_changed"),
				QJsonObject{{QStringLiteral("devices"), items}});
		});
	connect(pRecentDeviceService, &KRecentDeviceService::devicesChanged,
		this, [this](const QVector<KRecentDevice> &devices)
		{
			QJsonArray items;
			for (const KRecentDevice &device : devices)
			{
				items.append(QJsonObject{
					{QStringLiteral("deviceId"), device.strDeviceId},
					{QStringLiteral("authenticatedDeviceId"),
						device.strAuthenticatedDeviceId},
					{QStringLiteral("name"), device.strDeviceName},
					{QStringLiteral("host"), device.strHost},
					{QStringLiteral("port"), device.nSignalingPort},
					{QStringLiteral("lastConnectedAtMs"),
						QString::number(device.nLastConnectedAtMs)},
					{QStringLiteral("incoming"), device.bIncoming}
				});
			}
			m_recentDevices = items;
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("recent.devices_changed"),
				QJsonObject{{QStringLiteral("devices"), items}});
		});
	connect(pSettingsService, &KApplicationSettingsService::settingsChanged,
		this, [this](const KApplicationSettings &settings)
		{
			m_applicationSettings = QJsonObject{
				{QStringLiteral("remoteAccessEnabled"), settings.bRemoteAccessEnabled},
				{QStringLiteral("approvalMode"), RemoteApprovalModeName(settings.approvalMode)},
				{QStringLiteral("approvalTimeoutSeconds"), settings.nApprovalTimeoutSeconds},
				{QStringLiteral("defaultListenPort"), settings.nDefaultListenPort},
				{QStringLiteral("themeId"), settings.strThemeId}
			};
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("settings.changed"), m_applicationSettings);
		});
	connect(pClipboardService, &KClipboardSyncService::syncStateChanged,
		this, [this](bool bEnabled, bool bAvailable, bool bActive, const QString &strStatus)
		{
			m_clipboardState = QJsonObject{
				{QStringLiteral("enabled"), bEnabled},
				{QStringLiteral("available"), bAvailable},
				{QStringLiteral("active"), bActive},
				{QStringLiteral("status"), strStatus}
			};
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("clipboard.state_changed"), m_clipboardState);
		});
	connect(pTerminalService, &KTerminalSessionService::stateChanged,
		this, [this](KTerminalState state, bool bAvailable, const QString &strStatus,
			const QString &strDeviceName, const QString &strDeviceSource)
		{
			m_terminalState = QJsonObject{
				{QStringLiteral("state"), TerminalStateName(state)},
				{QStringLiteral("available"), bAvailable},
				{QStringLiteral("status"), strStatus},
				{QStringLiteral("deviceName"), strDeviceName},
				{QStringLiteral("deviceSource"), strDeviceSource}
			};
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("terminal.state_changed"), m_terminalState);
		});
	connect(pTerminalService, &KTerminalSessionService::outputReady,
		this, [this](const QByteArray &data)
		{
			appendEvent(TelemetryAutomationEventCategory,
				QStringLiteral("terminal.output"), QJsonObject{
					{QStringLiteral("dataBase64"), QString::fromLatin1(data.toBase64())},
					{QStringLiteral("byteCount"), QString::number(data.size())}
				});
		});
	connect(pTerminalService, &KTerminalSessionService::incomingRequest,
		this, [this](const QString &strRequestId, const QString &strDeviceName,
			const QString &strDeviceSource, qint64 nExpiresAtMs)
		{
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("terminal.access_requested"), QJsonObject{
					{QStringLiteral("requestId"), strRequestId},
					{QStringLiteral("deviceName"), strDeviceName},
					{QStringLiteral("deviceSource"), strDeviceSource},
					{QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs)}
				});
		});
	connect(pFileTransferService, &KFileTransferSessionService::stateChanged,
		this, [this](KFileTransferState state, bool bAvailable, const QString &strStatus)
		{
			m_fileTransferState = QJsonObject{
				{QStringLiteral("state"), FileTransferStateName(state)},
				{QStringLiteral("available"), bAvailable},
				{QStringLiteral("status"), strStatus}
			};
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("file_transfer.state_changed"), m_fileTransferState);
		});
	connect(pFileTransferService, &KFileTransferSessionService::paneChanged,
		this, [this](const KFileTransferPaneSnapshot &snapshot)
		{
			QJsonArray entries;
			for (const KFileTransferPaneEntry &entry : snapshot.entryList)
			{
				entries.append(QJsonObject{
					{QStringLiteral("entryId"), entry.strEntryId},
					{QStringLiteral("name"), entry.strName},
					{QStringLiteral("kind"), FileEntryKind(entry.type)},
					{QStringLiteral("sizeBytes"), QString::number(entry.nSize)},
					{QStringLiteral("modifiedAtMs"),
						QString::number(entry.lastModifiedUtc.toMSecsSinceEpoch())},
					{QStringLiteral("navigable"), entry.bNavigable},
					{QStringLiteral("transferable"), entry.bTransferable}
				});
			}
			QJsonObject pane{
				{QStringLiteral("pane"), FilePaneName(snapshot.pane)},
				{QStringLiteral("requestId"), snapshot.strRequestId},
				{QStringLiteral("listingId"), snapshot.strListingId},
				{QStringLiteral("displayPath"), snapshot.strDisplayPath},
				{QStringLiteral("canGoUp"), snapshot.bCanGoUp},
				{QStringLiteral("entries"), entries}
			};
			if (snapshot.pane == RemoteFileTransferPane)
				m_remoteFilePane = pane;
			else
				m_localFilePane = pane;
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("file_transfer.pane_changed"), pane);
		});
	connect(pFileTransferService, &KFileTransferSessionService::snapshotChanged,
		this, [this](const QVector<KFileTransferTaskSnapshot> &taskList)
		{
			QJsonArray tasks;
			for (const KFileTransferTaskSnapshot &task : taskList)
				tasks.append(FileTaskObject(task));
			m_fileTransferTasks = tasks;
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("file_transfer.tasks_changed"),
				QJsonObject{{QStringLiteral("tasks"), tasks}});
		});
	connect(pFileTransferService, &KFileTransferSessionService::taskChanged,
		this, [this, pFileTransferService](const KFileTransferTaskSnapshot &)
		{ pFileTransferService->requestSnapshot(); });
	connect(pFileTransferService, &KFileTransferSessionService::taskRemoved,
		this, [pFileTransferService](const QString &)
		{ pFileTransferService->requestSnapshot(); });
	connect(pFileTransferService, &KFileTransferSessionService::conflictRequested,
		this, [this](const KFileTransferConflictSnapshot &conflict)
		{
			m_fileTransferConflict = QJsonObject{
				{QStringLiteral("conflictId"), conflict.strConflictId},
				{QStringLiteral("taskId"), conflict.strTaskId},
				{QStringLiteral("fileId"), conflict.strFileId},
				{QStringLiteral("name"), conflict.strName},
				{QStringLiteral("sourceSizeBytes"), QString::number(conflict.nSourceSize)},
				{QStringLiteral("destinationSizeBytes"),
					QString::number(conflict.nDestinationSize)},
				{QStringLiteral("applyToRemainingAllowed"),
					conflict.bApplyToRemainingAllowed}
			};
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("file_transfer.conflict_requested"),
				m_fileTransferConflict);
		});
	connect(pSessionCoordinator, &KSessionCoordinator::trustedDevicesChanged,
		this, [this](const QVector<KTrustedDevice> &devices)
		{
			QJsonArray items;
			for (const KTrustedDevice &device : devices)
			{
				items.append(QJsonObject{
					{QStringLiteral("deviceId"), device.strDeviceId},
					{QStringLiteral("name"), device.strAlias.isEmpty()
						? device.strAdvertisedName : device.strAlias},
					{QStringLiteral("permissions"), QJsonArray::fromStringList(
						PermissionScopeNames(device.permissionLimit))},
					{QStringLiteral("revoked"), device.bRevoked}
				});
			}
			m_trustedDevices = items;
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("trusted.devices_changed"),
				QJsonObject{{QStringLiteral("devices"), items}});
		});

	m_applicationSettings = QJsonObject{
		{QStringLiteral("remoteAccessEnabled"), pSettingsService->settings().bRemoteAccessEnabled},
		{QStringLiteral("approvalMode"),
			RemoteApprovalModeName(pSettingsService->settings().approvalMode)},
		{QStringLiteral("approvalTimeoutSeconds"),
			pSettingsService->settings().nApprovalTimeoutSeconds},
		{QStringLiteral("defaultListenPort"),
			pSettingsService->settings().nDefaultListenPort},
		{QStringLiteral("themeId"), pSettingsService->settings().strThemeId}
	};
	pRecentDeviceService->requestDevices();
	pClipboardService->requestState();
	pTerminalService->requestState();
	pFileTransferService->requestSnapshot();
	pSessionCoordinator->requestTrustedDevices();
}

void KAutomationHostBridge::setHostReady()
{
	m_bHostReady.store(true);
}

void KAutomationHostBridge::stopAcceptingRequests()
{
	m_bAcceptingRequests = false;
}

void KAutomationHostBridge::SubmitCommand(void *pHostContext,
	std::uint64_t nRequestId,
	const char *pCommandIdUtf8,
	std::uint32_t nCommandIdBytes,
	const char *pArgumentsJsonUtf8,
	std::uint32_t nArgumentsJsonBytes,
	std::uint32_t nTimeoutMs,
	KWrcDriverCommandStartedCallback pStartedCallback,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	auto *pBridge = static_cast<KAutomationHostBridge *>(pHostContext);
	if (pBridge == nullptr || pCallback == nullptr)
		return;
	pBridge->submitCommand(nRequestId,
		BytesFromRaw(pCommandIdUtf8, nCommandIdBytes),
		BytesFromRaw(pArgumentsJsonUtf8, nArgumentsJsonBytes),
		nTimeoutMs, pStartedCallback, pCallback, pCallbackContext);
}

void KAutomationHostBridge::RequestSnapshot(void *pHostContext,
	std::uint64_t nRequestId,
	const char *pSnapshotKindUtf8,
	std::uint32_t nSnapshotKindBytes,
	std::uint64_t nSinceSequence,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	auto *pBridge = static_cast<KAutomationHostBridge *>(pHostContext);
	if (pBridge == nullptr || pCallback == nullptr)
		return;
	pBridge->requestSnapshot(nRequestId,
		BytesFromRaw(pSnapshotKindUtf8, nSnapshotKindBytes),
		nSinceSequence, pCallback, pCallbackContext);
}

std::uint32_t KAutomationHostBridge::CopyHostValue(void *pHostContext,
	const char *pKeyUtf8,
	std::uint32_t nKeyBytes,
	char *pDestinationUtf8,
	std::uint32_t nDestinationBytes)
{
	const auto *pBridge = static_cast<const KAutomationHostBridge *>(pHostContext);
	if (pBridge == nullptr)
		return 0;
	const QString strKey = QString::fromUtf8(pKeyUtf8, static_cast<qsizetype>(nKeyBytes));
	QByteArray value;
	if (strKey == QStringLiteral("pid"))
		value = QByteArray::number(QCoreApplication::applicationPid());
	else if (strKey == QStringLiteral("buildId"))
		value = QByteArray(KWrcAutomationBuildId);
	else if (strKey == QStringLiteral("dataDirectory"))
		value = pBridge->m_strDataDirectory.toUtf8();
	else if (strKey == QStringLiteral("eventCursor"))
		value = QByteArray::number(pBridge->m_nPublishedEventCursor.load());
	else if (strKey == QStringLiteral("sessionGeneration"))
		value = QByteArray::number(pBridge->m_nObservedSessionGeneration.load());
	else
		return 0;

	const std::uint32_t nRequired = static_cast<std::uint32_t>(value.size() + 1);
	if (pDestinationUtf8 == nullptr || nDestinationBytes < nRequired)
		return nRequired;
	std::memcpy(pDestinationUtf8, value.constData(), static_cast<size_t>(value.size()));
	pDestinationUtf8[value.size()] = '\0';
	return nRequired;
}

bool KAutomationHostBridge::IsHostReady(void *pHostContext)
{
	const auto *pBridge = static_cast<const KAutomationHostBridge *>(pHostContext);
	return pBridge != nullptr && pBridge->m_bHostReady.load();
}

void KAutomationHostBridge::WriteLog(void *pHostContext,
	std::uint32_t nLevel,
	const char *pMessageUtf8,
	std::uint32_t nMessageBytes)
{
	if (pHostContext == nullptr)
		return;
	const QString strMessage = QString::fromUtf8(
		pMessageUtf8, static_cast<qsizetype>(nMessageBytes));
	if (nLevel >= 2)
		qWarning().noquote() << QStringLiteral("Automation driver: %1").arg(strMessage);
	else
		qInfo().noquote() << QStringLiteral("Automation driver: %1").arg(strMessage);
}

void KAutomationHostBridge::submitCommand(quint64 nRequestId,
	const QByteArray &commandIdUtf8,
	const QByteArray &argumentsJsonUtf8,
	quint32 nTimeoutMs,
	KWrcDriverCommandStartedCallback pStartedCallback,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	const quint64 nSubmittedGeneration = m_nObservedSessionGeneration.load();
	const QDeadlineTimer deadline(static_cast<qint64>(nTimeoutMs), Qt::PreciseTimer);
	QMetaObject::invokeMethod(this,
		[this, nRequestId, nSubmittedGeneration, commandIdUtf8, argumentsJsonUtf8,
			deadline, pStartedCallback, pCallback, pCallbackContext]()
		{
			if (m_seenCommandRequestIds.contains(nRequestId))
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("duplicate_request_id"),
						QStringLiteral("Host request id was already processed")),
					pCallback, pCallbackContext);
				return;
			}
			m_seenCommandRequestIds.insert(nRequestId);
			m_commandRequestOrder.append(nRequestId);
			if (m_commandRequestOrder.size() > 4096)
				m_seenCommandRequestIds.remove(m_commandRequestOrder.takeFirst());
			if (!m_bAcceptingRequests)
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("application_shutdown"),
						QStringLiteral("Application is shutting down")),
					pCallback, pCallbackContext);
				return;
			}
			if (nSubmittedGeneration != m_pSessionController->sessionGeneration())
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("stale_generation"),
						QStringLiteral("Remote session changed before command execution")),
					pCallback, pCallbackContext);
				return;
			}
			if (deadline.hasExpired())
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("command_timeout"),
						QStringLiteral("Command expired before execution")),
					pCallback, pCallbackContext);
				appendEvent(StateAutomationEventCategory,
					QStringLiteral("command.expired"), QJsonObject{
					{QStringLiteral("requestId"), QString::number(nRequestId)},
					{QStringLiteral("commandId"), QString::fromUtf8(commandIdUtf8)}
				});
				return;
			}

			QJsonParseError parseError;
			const QJsonDocument document = QJsonDocument::fromJson(argumentsJsonUtf8, &parseError);
			if (parseError.error != QJsonParseError::NoError || !document.isObject())
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("invalid_argument"),
						QStringLiteral("Command arguments must be a JSON object")),
					pCallback, pCallbackContext);
				return;
			}

			const QString strCommandId = QString::fromUtf8(commandIdUtf8);
			if (pStartedCallback != nullptr)
				pStartedCallback(pCallbackContext, nRequestId);
			const KApplicationCommandResult result = m_pRegistry->execute(
				strCommandId, document.object());
			QJsonObject response{
				{QStringLiteral("status"), static_cast<int>(result.status)},
				{QStringLiteral("value"), result.value},
				{QStringLiteral("errorCode"), result.strErrorCode},
				{QStringLiteral("technicalMessage"), result.strTechnicalMessage}
			};
			completeJson(nRequestId, response, pCallback, pCallbackContext);
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("command.completed"), QJsonObject{
				{QStringLiteral("requestId"), QString::number(nRequestId)},
				{QStringLiteral("commandId"), strCommandId},
				{QStringLiteral("status"), static_cast<int>(result.status)}
			});
		}, Qt::QueuedConnection);
}

void KAutomationHostBridge::requestSnapshot(quint64 nRequestId,
	const QByteArray &snapshotKindUtf8,
	quint64 nSinceSequence,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext)
{
	QMetaObject::invokeMethod(this,
		[this, nRequestId, snapshotKindUtf8, nSinceSequence, pCallback, pCallbackContext]()
		{
			if (!m_bAcceptingRequests)
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("application_shutdown"),
						QStringLiteral("Application is shutting down")),
					pCallback, pCallbackContext);
				return;
			}
			const QString strKind = QString::fromUtf8(snapshotKindUtf8);
			if (strKind == QStringLiteral("state"))
				completeJson(nRequestId, stateSnapshot(), pCallback, pCallbackContext);
			else if (strKind == QStringLiteral("events"))
				completeJson(nRequestId, eventsSnapshot(nSinceSequence), pCallback, pCallbackContext);
			else
			{
				completeJson(nRequestId,
					ErrorObject(QStringLiteral("invalid_argument"),
						QStringLiteral("Unknown snapshot kind")),
					pCallback, pCallbackContext);
			}
		}, Qt::QueuedConnection);
}

void KAutomationHostBridge::synchronizeSessionGeneration()
{
	const quint64 nGeneration = m_pSessionController->sessionGeneration();
	const quint64 nPreviousGeneration = m_nObservedSessionGeneration.exchange(nGeneration);
	if (nPreviousGeneration == nGeneration)
		return;

	m_strSignalingState.clear();
	m_strWebRtcState.clear();
	m_strRemoteDeviceId.clear();
	m_currentError = QJsonObject();
	m_negotiatedCapabilities.clear();
	m_privacyModeStatus = KPrivacyModeStatus();
	m_postSessionActionStatus = KPostSessionActionStatus();
	m_nReceivedFrameCount = 0;
	m_nLastFrameTimestampMs = 0;
	m_nLastFrameWidth = 0;
	m_nLastFrameHeight = 0;
	m_bSessionChannelOpen = false;
	m_bInputChannelOpen = false;
	m_bClipboardChannelOpen = false;
	m_bTerminalChannelOpen = false;
}

void KAutomationHostBridge::initializeStateConnections()
{
	m_strSessionState = KSessionStateMachine::stateName(
		m_pSessionController->isIdle() ? IdleSessionState : ConnectedSessionState);
	connect(m_pSessionController, &KSessionController::listeningAvailabilityChanged,
		this, [this](bool bAvailable, quint16 nPort)
		{
			m_bListeningAvailable = bAvailable;
			m_nListeningPort = nPort;
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("signaling.listening_changed"), QJsonObject{
				{QStringLiteral("available"), bAvailable},
				{QStringLiteral("port"), nPort}
			});
		});
	connect(m_pSessionController, &KSessionController::signalingChanged,
		this, [this](const QString &strState)
		{
			synchronizeSessionGeneration();
			m_strSignalingState = strState;
		});
	connect(m_pSessionController, &KSessionController::webRtcStateChanged,
		this, [this](const QString &strState)
		{
			synchronizeSessionGeneration();
			m_strWebRtcState = strState;
		});
	connect(m_pSessionController, &KSessionController::sessionStateChanged,
		this, [this](KSessionState state)
		{
			synchronizeSessionGeneration();
			m_strSessionState = KSessionStateMachine::stateName(state);
			if (state == ConnectedSessionState || state == StreamingSessionState)
			{
				m_strWebRtcState = QStringLiteral("connected");
				m_currentError = QJsonObject();
			}
			else if (state == ReconnectingSessionState)
				m_strWebRtcState = QStringLiteral("disconnected");
			appendEvent(StateAutomationEventCategory,
				QStringLiteral("session.state_changed"),
				QJsonObject{{QStringLiteral("state"), m_strSessionState}});
		});
	connect(m_pSessionController, &KSessionController::sessionChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bSessionChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::inputChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bInputChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::clipboardChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bClipboardChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::terminalChannelChanged,
		this, [this](bool bOpen)
		{
			synchronizeSessionGeneration();
			m_bTerminalChannelOpen = bOpen;
		});
	connect(m_pSessionController, &KSessionController::sessionCapabilitiesChanged,
		this, [this](const KNegotiatedCapabilities &capabilities)
		{
			synchronizeSessionGeneration();
			m_negotiatedCapabilities = capabilities.channels;
			if (capabilities.bFileTransfer)
				m_negotiatedCapabilities.append(QStringLiteral("fileTransfer"));
		});
	connect(m_pSessionController, &KSessionController::remoteFrameStatsReady,
		this, [this](int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs)
		{
			Q_UNUSED(nFrameIndex);
			Q_UNUSED(nTimestampMs);
			synchronizeSessionGeneration();
			const qint64 nReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
			++m_nReceivedFrameCount;
			m_nLastFrameWidth = nWidth;
			m_nLastFrameHeight = nHeight;
			m_nLastFrameTimestampMs = nReceivedAtMs;
			if (!m_frameProgressEventTimer.isValid()
				|| m_frameProgressEventTimer.elapsed() >= 1000)
			{
				m_frameProgressEventTimer.restart();
				appendEvent(TelemetryAutomationEventCategory,
					QStringLiteral("frame.progress"), QJsonObject{
						{QStringLiteral("frameCount"), QString::number(m_nReceivedFrameCount)},
						{QStringLiteral("lastFrameTimestampMs"), QString::number(nReceivedAtMs)}
					});
			}
		});
	connect(m_pSessionController, &KSessionController::incomingAccessRequest,
		this, [this](const QString &strRequestId, const QString &strDeviceName,
			const QString &, qint64 nExpiresAtMs)
		{
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("access.requested"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("deviceName"), strDeviceName},
				{QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs)}
			});
		});
	connect(m_pSessionController, &KSessionController::incomingAccessRequestCleared,
		this, [this](const QString &strRequestId, const QString &strReason)
		{
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("access.cleared"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("reason"), strReason}
			});
		});
	connect(m_pSessionController, &KSessionController::pairingRequested,
		this, [this](const QString &strRequestId, const QString &strDeviceName,
			const QString &strLocalRole, const QString &strVerificationCode,
			const QString &, const QString &, const QString &, const QString &,
			KPermissionScopes requestedPermissions, qint64 nExpiresAtMs)
		{
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("pairing.requested"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("deviceName"), strDeviceName},
				{QStringLiteral("localRole"), strLocalRole},
				{QStringLiteral("verificationCode"), strVerificationCode},
				{QStringLiteral("requestedPermissions"),
					QString::number(static_cast<quint32>(requestedPermissions))},
				{QStringLiteral("expiresAtMs"), QString::number(nExpiresAtMs)}
			});
		});
	connect(m_pSessionController, &KSessionController::pairingCleared,
		this, [this](const QString &strRequestId, const QString &strReason)
		{
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("pairing.cleared"), QJsonObject{
				{QStringLiteral("requestId"), strRequestId},
				{QStringLiteral("reason"), strReason}
			});
		});
	connect(m_pSessionController, &KSessionController::deviceAuthenticationStateChanged,
		this, [this](const QString &, const QString &strDeviceId, const QString &, bool)
		{
			synchronizeSessionGeneration();
			m_strRemoteDeviceId = strDeviceId;
		});
	connect(m_pSessionController, &KSessionController::privacyModeStatusChanged,
		this, [this](const KPrivacyModeStatus &status)
		{
			synchronizeSessionGeneration();
			m_privacyModeStatus = status;
		});
	connect(m_pSessionController, &KSessionController::postSessionActionStatusChanged,
		this, [this](const KPostSessionActionStatus &status)
		{
			synchronizeSessionGeneration();
			m_postSessionActionStatus = status;
		});
	connect(m_pSessionController, &KSessionController::privacyModeCommandCompleted,
		this, [this](const QString &, bool bSuccess, const QString &strErrorCode)
		{
			synchronizeSessionGeneration();
			if (!bSuccess)
				m_privacyModeStatus.strErrorCode = strErrorCode;
		});
	connect(m_pSessionController, &KSessionController::postSessionActionCommandCompleted,
		this, [this](const QString &, bool bSuccess, const QString &strErrorCode)
		{
			synchronizeSessionGeneration();
			if (!bSuccess)
				m_postSessionActionStatus.strErrorCode = strErrorCode;
		});
	connect(m_pSessionController, &KSessionController::sessionErrorOccurred,
		this, [this](const KSessionError &error)
		{
			synchronizeSessionGeneration();
			m_currentError = sessionErrorObject(error);
			m_lastError = m_currentError;
			appendEvent(CriticalAutomationEventCategory,
				QStringLiteral("session.error"), m_currentError);
		});
}

void KAutomationHostBridge::appendEvent(KAutomationEventCategory category,
	const QString &strType,
	const QJsonObject &value)
{
	synchronizeSessionGeneration();
	KAutomationEvent event;
	event.nSequence = m_nNextEventSequence++;
	event.category = category;
	event.strType = strType;
	event.value = value;
	event.value.insert(QStringLiteral("sessionGeneration"),
		QString::number(m_nObservedSessionGeneration.load()));
	m_events.append(event);

	auto categoryCount = [this](KAutomationEventCategory target)
	{
		int nCount = 0;
		for (const KAutomationEvent &candidate : m_events)
		{
			if (candidate.category == target)
				++nCount;
		}
		return nCount;
	};
	auto removeOldestCategory = [this](KAutomationEventCategory target)
	{
		for (auto iter = m_events.begin(); iter != m_events.end(); ++iter)
		{
			if (iter->category == target)
			{
				m_events.erase(iter);
				return true;
			}
		}
		return false;
	};
	const int nCategoryLimit = category == CriticalAutomationEventCategory
		? kMaximumCriticalAutomationEvents
		: (category == StateAutomationEventCategory
			? kMaximumStateAutomationEvents : kMaximumTelemetryAutomationEvents);
	while (categoryCount(category) > nCategoryLimit)
		removeOldestCategory(category);
	while (m_events.size() > kMaximumAutomationEvents)
	{
		if (!removeOldestCategory(TelemetryAutomationEventCategory)
			&& !removeOldestCategory(StateAutomationEventCategory))
		{
			m_events.removeFirst();
		}
	}
	m_nPublishedEventCursor.store(event.nSequence);
}

QJsonObject KAutomationHostBridge::sessionErrorObject(const KSessionError &error) const
{
	return QJsonObject{
		{QStringLiteral("code"), KSessionError::codeName(error.code)},
		{QStringLiteral("domain"), KSessionError::domainName(error.domain)},
		{QStringLiteral("stage"), KSessionError::stageName(error.stage)},
		{QStringLiteral("retryable"), error.bRetryable},
		{QStringLiteral("technicalMessage"), error.strTechnicalMessage},
		{QStringLiteral("occurredAtMs"),
			QString::number(QDateTime::currentMSecsSinceEpoch())},
		{QStringLiteral("sessionGeneration"),
			QString::number(m_nObservedSessionGeneration.load())}
	};
}

QJsonObject KAutomationHostBridge::stateSnapshot() const
{
	QJsonArray capabilities;
	for (const QString &strCapability : m_negotiatedCapabilities)
		capabilities.append(strCapability);
	QJsonArray supportedCommands;
	for (const QString &strCommandId : m_pRegistry->commandIds())
		supportedCommands.append(strCommandId);
	return QJsonObject{
		{QStringLiteral("supportedCommands"), supportedCommands},
		{QStringLiteral("role"), KSessionStateMachine::roleName(m_pSessionController->sessionRole())},
		{QStringLiteral("sessionGeneration"),
			QString::number(m_nObservedSessionGeneration.load())},
		{QStringLiteral("sessionState"), m_strSessionState},
		{QStringLiteral("signalingState"), m_strSignalingState},
		{QStringLiteral("webRtcState"), m_strWebRtcState},
		{QStringLiteral("listeningAvailable"), m_bListeningAvailable},
		{QStringLiteral("listeningPort"), m_nListeningPort},
		{QStringLiteral("sessionChannelOpen"), m_bSessionChannelOpen},
		{QStringLiteral("inputChannelOpen"), m_bInputChannelOpen},
		{QStringLiteral("clipboardChannelOpen"), m_bClipboardChannelOpen},
		{QStringLiteral("terminalChannelOpen"), m_bTerminalChannelOpen},
		{QStringLiteral("remoteDeviceId"), m_strRemoteDeviceId},
		{QStringLiteral("negotiatedCapabilities"), capabilities},
		{QStringLiteral("lanDevices"), m_lanDevices},
		{QStringLiteral("recentDevices"), m_recentDevices},
		{QStringLiteral("trustedDevices"), m_trustedDevices},
		{QStringLiteral("applicationSettings"), m_applicationSettings},
		{QStringLiteral("clipboard"), m_clipboardState},
		{QStringLiteral("terminal"), m_terminalState},
		{QStringLiteral("fileTransfer"), m_fileTransferState},
		{QStringLiteral("localFilePane"), m_localFilePane.isEmpty()
			? QJsonValue(QJsonValue::Null) : QJsonValue(m_localFilePane)},
		{QStringLiteral("remoteFilePane"), m_remoteFilePane.isEmpty()
			? QJsonValue(QJsonValue::Null) : QJsonValue(m_remoteFilePane)},
		{QStringLiteral("fileTransferTasks"), m_fileTransferTasks},
		{QStringLiteral("fileTransferConflict"), m_fileTransferConflict.isEmpty()
			? QJsonValue(QJsonValue::Null) : QJsonValue(m_fileTransferConflict)},
		{QStringLiteral("receivedFrameCount"), QString::number(m_nReceivedFrameCount)},
		{QStringLiteral("lastFrameWidth"), m_nLastFrameWidth},
		{QStringLiteral("lastFrameHeight"), m_nLastFrameHeight},
		{QStringLiteral("lastFrameTimestampMs"), QString::number(m_nLastFrameTimestampMs)},
		{QStringLiteral("privacyModeStatus"), QJsonObject{
			{QStringLiteral("requestedMode"), PrivacyModeName(m_privacyModeStatus.requestedMode)},
			{QStringLiteral("effectiveMode"), PrivacyModeName(m_privacyModeStatus.effectiveMode)},
			{QStringLiteral("state"), PrivacyStateName(m_privacyModeStatus.state)},
			{QStringLiteral("errorCode"), m_privacyModeStatus.strErrorCode}
		}},
		{QStringLiteral("postSessionActionStatus"), QJsonObject{
			{QStringLiteral("action"), PostSessionActionName(m_postSessionActionStatus.action)},
			{QStringLiteral("errorCode"), m_postSessionActionStatus.strErrorCode}
		}},
		{QStringLiteral("currentError"), m_currentError.isEmpty()
			? QJsonValue(QJsonValue::Null) : QJsonValue(m_currentError)},
		{QStringLiteral("lastError"), m_lastError.isEmpty()
			? QJsonValue(QJsonValue::Null) : QJsonValue(m_lastError)}
	};
}

QJsonObject KAutomationHostBridge::eventsSnapshot(quint64 nSinceSequence) const
{
	QJsonArray events;
	quint64 nExpectedSequence = nSinceSequence + 1;
	bool bHasGap = false;
	for (const KAutomationEvent &event : m_events)
	{
		if (event.nSequence <= nSinceSequence)
			continue;
		if (event.nSequence != nExpectedSequence)
			bHasGap = true;
		nExpectedSequence = event.nSequence + 1;
		QJsonObject item = event.value;
		item.insert(QStringLiteral("sequence"), QString::number(event.nSequence));
		item.insert(QStringLiteral("type"), event.strType);
		item.insert(QStringLiteral("category"), event.category == CriticalAutomationEventCategory
			? QStringLiteral("critical")
			: (event.category == StateAutomationEventCategory
				? QStringLiteral("state") : QStringLiteral("telemetry")));
		events.append(item);
	}
	if (nExpectedSequence < m_nNextEventSequence)
		bHasGap = true;
	const quint64 nOldest = m_events.isEmpty() ? m_nNextEventSequence : m_events.first().nSequence;
	return QJsonObject{
		{QStringLiteral("events"), events},
		{QStringLiteral("oldestSequence"), QString::number(nOldest)},
		{QStringLiteral("nextSequence"), QString::number(m_nNextEventSequence)},
		{QStringLiteral("hasGap"), bHasGap}
	};
}

void KAutomationHostBridge::completeJson(quint64 nRequestId,
	const QJsonObject &object,
	KWrcDriverJsonCallback pCallback,
	void *pCallbackContext) const
{
	if (pCallback == nullptr)
		return;
	const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
	pCallback(pCallbackContext, nRequestId, json.constData(),
		static_cast<std::uint32_t>(json.size()));
}
