#include "automation/automationhostbridge.h"

#include "clipboard/clipboardsyncservice.h"
#include "core/devices/recentdevice.h"
#include "core/discovery/discovereddevice.h"
#include "core/file_transfer/filetransferstate.h"
#include "core/security/permissionscope.h"
#include "core/security/trusteddevice.h"
#include "devices/recentdeviceservice.h"
#include "file_transfer/filetransfersessionservice.h"
#include "session/sessioncoordinator.h"
#include "settings/applicationsettingsservice.h"
#include "terminal/terminalsessionservice.h"
#include "ui_bridge/devicediscoveryviewmodel.h"

#include <QtCore/QJsonArray>

namespace
{
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
