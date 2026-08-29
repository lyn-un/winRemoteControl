#include "app/composition/applicationcomposition.h"

#include "adapters/windows/device/windowsdeviceinfoprovider.h"
#include "adapters/windows/input/windowsinputinjector.h"
#include "adapters/windows/file_transfer/windowsfilesystemadapter.h"
#include "adapters/windows/privacy/windowsdisplaypoweradapter.h"
#include "adapters/windows/privacy/windowsprivacyoverlayadapter.h"
#include "adapters/windows/privacy/windowsworkstationlockadapter.h"
#include "adapters/windows/security/signedjsontrusteddevicestore.h"
#include "adapters/windows/security/windowsdeviceidentityprovider.h"
#include "adapters/clipboard/qtclipboardadapter.h"
#include "adapters/signaling/tcpsignalingtransport.h"
#include "adapters/discovery/udplandiscoverytransport.h"
#include "adapters/settings/qsettingsrecentdevicestore.h"
#include "adapters/settings/qsettingsapplicationstore.h"
#include "adapters/settings/qsettingsdevicesecuritypreferencestore.h"
#include "app/remotedesktopwindow.h"
#include "app/filetransferwindow.h"
#include "adapters/windows/terminal/windowspseudoconsole.h"
#include "adapters/windows/terminal/windowsterminalfrontend.h"
#include "capture/captureservice.h"
#include "commands/applicationcommandregistry.h"
#include "commands/builtincommands.h"
#include "automation/automationhostbridge.h"
#include "automation/automationpluginloader.h"
#include "core/discovery/devicediscoverycontroller.h"
#include "discovery/landiscoveryservice.h"
#include "devices/recentdeviceservice.h"
#include "render/videorenderwidget.h"
#include "session/sessionviewmodel.h"
#include "session/sessionerrorpresenter.h"
#include "transport/webrtc/webrtcpeer.h"
#include "session/sessioncoordinator.h"
#include "settings/applicationsettingsservice.h"
#include "settings/devicesecuritypreferenceservice.h"
#include "clipboard/clipboardsyncservice.h"
#include "terminal/terminalsessionservice.h"
#include "file_transfer/filetransfersessionservice.h"
#include "privacy/postsessionactionservice.h"
#include "privacy/privacymodeservice.h"
#include "ui_bridge/webviewwidget.h"
#include "ui_bridge/devicediscoveryviewmodel.h"
#include "common/sessiontracelogger.h"
#include "common/applicationpaths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QSysInfo>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <QtCore/QTimer>
#include <memory>

namespace
{
	constexpr int kApplicationShutdownDeadlineMs = 8000;

	QString RecentDevicesFilePath()
	{
		const QString strFileName = QStringLiteral("recent_devices.ini");
		const QString strFilePath = KApplicationPaths::dataFilePath(strFileName);
		const QString strLegacyFilePath =
			QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
				.filePath(strFileName);
		if (strLegacyFilePath != strFilePath
			&& QFileInfo::exists(strLegacyFilePath)
			&& !QFileInfo::exists(strFilePath)
			&& QFile::copy(strLegacyFilePath, strFilePath))
		{
			QFile::remove(strLegacyFilePath);
		}
		return strFilePath;
	}

	QString ApplicationSettingsFilePath()
	{
		return KApplicationPaths::dataFilePath(QStringLiteral("settings.ini"));
	}

	QString DeviceSecurityPreferencesFilePath()
	{
		return KApplicationPaths::dataFilePath(
			QStringLiteral("device_security_preferences.ini"));
	}

	QString SecurityDirectoryPath()
	{
		return KApplicationPaths::dataFilePath(QStringLiteral("security"));
	}

	QString TrustedDevicesFilePath()
	{
		return QDir(SecurityDirectoryPath())
			.filePath(QStringLiteral("trusted_devices.json"));
	}

	bool ParseFileTransferPane(const QString &strPane, KFileTransferPane *pPane)
	{
		if (pPane == nullptr)
			return false;
		if (strPane == QStringLiteral("local"))
		{
			*pPane = LocalFileTransferPane;
			return true;
		}
		if (strPane == QStringLiteral("remote"))
		{
			*pPane = RemoteFileTransferPane;
			return true;
		}
		return false;
	}

	KFileTransferConflictResolution FileTransferConflictResolutionFromName(
		const QString &strResolution)
	{
		if (strResolution == QStringLiteral("overwrite"))
			return OverwriteFileTransferConflictResolution;
		if (strResolution == QStringLiteral("keepBoth"))
			return KeepBothFileTransferConflictResolution;
		if (strResolution == QStringLiteral("skip"))
			return SkipFileTransferConflictResolution;
		return InvalidFileTransferConflictResolution;
	}
}

KApplicationComposition::KApplicationComposition(QObject *pParent)
	: QObject(pParent)
	, m_pCaptureService(new KCaptureService(this))
	, m_pApplicationCommandRegistry(new KApplicationCommandRegistry(this))
	, m_pSessionService(new KSessionCoordinator(
		std::make_unique<KWindowsDeviceInfoProvider>(),
		std::make_unique<KWindowsInputInjector>(),
		std::make_unique<KWebRtcPeer>(),
		std::make_unique<KTcpSignalingTransport>(),
		std::make_unique<KWindowsDeviceIdentityProvider>(
			SecurityDirectoryPath(), KApplicationPaths::isAutomationTestProfile()),
		std::make_unique<KSignedJsonTrustedDeviceStore>(TrustedDevicesFilePath()),
		this))
	, m_pSessionViewModel(new KSessionViewModel(
		m_pCaptureService,
		m_pSessionService,
		this))
	, m_pDiscoveryService(new KLanDiscoveryService(
		std::make_unique<KUdpLanDiscoveryTransport>(),
		QUuid::createUuid().toString(QUuid::WithoutBraces),
		QSysInfo::machineHostName(),
		this))
	, m_pDiscoveryViewModel(new KDeviceDiscoveryViewModel(
		m_pDiscoveryService,
		this))
	, m_pRecentDeviceService(new KRecentDeviceService(
		std::make_unique<KQSettingsRecentDeviceStore>(
			RecentDevicesFilePath()),
		this))
	, m_pApplicationSettingsService(new KApplicationSettingsService(
		std::make_unique<KQSettingsApplicationStore>(ApplicationSettingsFilePath()),
		this))
	, m_pDeviceSecurityPreferenceService(new KDeviceSecurityPreferenceService(
		std::make_unique<KQSettingsDeviceSecurityPreferenceStore>(
			DeviceSecurityPreferencesFilePath()),
		m_pSessionService,
		this))
	, m_pClipboardSyncService(new KClipboardSyncService(
		std::make_unique<KQtClipboardAdapter>(),
		m_pSessionService,
		this))
	, m_pTerminalSessionService(new KTerminalSessionService(
		std::make_unique<KWindowsPseudoConsole>(),
		m_pSessionService,
		std::make_unique<KWindowsTerminalFrontend>(),
		this))
	, m_pFileTransferSessionService(new KFileTransferSessionService(
		std::make_unique<KWindowsFileSystemAdapter>(),
		m_pSessionService,
		this))
	, m_pShutdownDeadlineTimer(new QTimer(this))
{
	if (!RegisterBuiltinApplicationCommands(
		m_pApplicationCommandRegistry,
		m_pSessionViewModel,
		m_pSessionService,
		m_pDeviceSecurityPreferenceService))
	{
		qFatal("Failed to register built-in application commands");
	}
	m_pAutomationHostBridge = new KAutomationHostBridge(
		m_pApplicationCommandRegistry,
		m_pSessionService,
		KApplicationPaths::dataDirectoryPath(),
		this);
	m_spAutomationPluginLoader = std::make_unique<KAutomationPluginLoader>();
	QString strAutomationError;
	if (!m_spAutomationPluginLoader->load(QCoreApplication::applicationDirPath(),
		m_pAutomationHostBridge->hostApi(), &strAutomationError))
	{
		qWarning().noquote() << QStringLiteral("Automation driver was not loaded: %1")
			.arg(strAutomationError);
	}
	else if (!m_spAutomationPluginLoader->isLoaded())
	{
		qInfo().noquote() << QStringLiteral("Automation driver is not present");
	}
	m_pSessionService->configurePrivacyServices(
		std::make_unique<KPrivacyModeService>(
			std::make_unique<KWindowsPrivacyOverlayAdapter>(),
			std::make_unique<KWindowsDisplayPowerAdapter>()),
		std::make_unique<KPostSessionActionService>(
			std::make_unique<KWindowsWorkstationLockAdapter>()));
	m_pSessionService->setTerminalCapabilitiesAvailable(
		m_pTerminalSessionService->isFrontendSupported(),
		m_pTerminalSessionService->isHostSupported());
	m_pShutdownDeadlineTimer->setSingleShot(true);
	m_pShutdownDeadlineTimer->setInterval(kApplicationShutdownDeadlineMs);
	connect(m_pShutdownDeadlineTimer, &QTimer::timeout,
		this, &KApplicationComposition::handleShutdownDeadline);
	m_pRecentDeviceService->initialize();
	m_pApplicationSettingsService->initialize();
	m_pDeviceSecurityPreferenceService->initialize();
	m_pSessionService->applyApplicationSettings(m_pApplicationSettingsService->settings());
	m_pTerminalSessionService->setApprovalTimeoutSeconds(
		m_pApplicationSettingsService->settings().nApprovalTimeoutSeconds);
	wireServices();
}

KApplicationComposition::~KApplicationComposition()
{
	shutdown();
}

KSessionViewModel *KApplicationComposition::sessionViewModel() const
{
	return m_pSessionViewModel;
}

KApplicationCommandRegistry *KApplicationComposition::applicationCommandRegistry() const
{
	return m_pApplicationCommandRegistry;
}

QString KApplicationComposition::applicationThemeId() const
{
	return m_pApplicationSettingsService->settings().strThemeId;
}

void KApplicationComposition::wireDashboard(KWebViewWidget *pWebViewWidget)
{
	Q_ASSERT(pWebViewWidget != nullptr);
	connect(pWebViewWidget, &KWebViewWidget::startCaptureRequested,
		m_pSessionViewModel, &KSessionViewModel::startLocalPreview);
	connect(pWebViewWidget, &KWebViewWidget::stopCaptureRequested,
		m_pSessionViewModel, &KSessionViewModel::stopCapture);
	connect(pWebViewWidget, &KWebViewWidget::setRoleRequested,
		this, [this](const QString &strRole)
		{
			const KApplicationCommandResult result = m_pApplicationCommandRegistry->execute(
				QStringLiteral("application.set_role"),
				QJsonObject{{QStringLiteral("role"), strRole}});
			if (result.status == ApplicationCommandSucceeded)
				m_pDiscoveryViewModel->setRole(strRole);
		});
	connect(pWebViewWidget, &KWebViewWidget::startSignalingServerRequested,
		m_pSessionViewModel, &KSessionViewModel::startSignalingServer);
	connect(pWebViewWidget, &KWebViewWidget::connectSignalingRequested,
		m_pRecentDeviceService, &KRecentDeviceService::connectEndpoint);
	connect(pWebViewWidget, &KWebViewWidget::retryLastConnectionRequested,
		m_pSessionViewModel, &KSessionViewModel::retryLastConnection);
	connect(pWebViewWidget, &KWebViewWidget::refreshLanDevicesRequested,
		m_pDiscoveryViewModel, &KDeviceDiscoveryViewModel::refreshLanDevices);
	connect(pWebViewWidget, &KWebViewWidget::connectLanDeviceRequested,
		m_pDiscoveryViewModel, &KDeviceDiscoveryViewModel::connectLanDevice);
	connect(pWebViewWidget, &KWebViewWidget::requestRecentDevicesRequested,
		m_pRecentDeviceService, &KRecentDeviceService::requestDevices);
	connect(pWebViewWidget, &KWebViewWidget::connectRecentDeviceRequested,
		m_pRecentDeviceService, &KRecentDeviceService::connectDevice);
	connect(pWebViewWidget, &KWebViewWidget::removeRecentDeviceRequested,
		m_pRecentDeviceService, &KRecentDeviceService::removeDevice);
	connect(pWebViewWidget, &KWebViewWidget::openRecentDeviceTerminalRequested,
		m_pRecentDeviceService, &KRecentDeviceService::openTerminalDevice);
	connect(pWebViewWidget, &KWebViewWidget::openCurrentTerminalRequested,
		m_pTerminalSessionService, [this]()
		{
			m_pTerminalSessionService->openCurrentTerminal();
		});
	connect(pWebViewWidget, &KWebViewWidget::requestTerminalFrontendSupportRequested,
		this, [this, pWebViewWidget]()
		{
			QString strReason;
			const bool bSupported = m_pTerminalSessionService->isFrontendSupported(&strReason);
			pWebViewWidget->sendTerminalFrontendSupportChanged(bSupported, strReason);
		});
	connect(pWebViewWidget, &KWebViewWidget::respondTerminalAccessRequestRequested,
		m_pTerminalSessionService, &KTerminalSessionService::respondIncomingRequest);
	connect(pWebViewWidget, &KWebViewWidget::closeTerminalRequested,
		m_pTerminalSessionService, &KTerminalSessionService::closeTerminal);
	connect(pWebViewWidget, &KWebViewWidget::openCurrentFileTransferRequested,
		m_pFileTransferSessionService,
		&KFileTransferSessionService::openCurrentFileTransfer);
	connect(pWebViewWidget, &KWebViewWidget::stopCurrentFileTransferRequested,
		m_pFileTransferSessionService,
		&KFileTransferSessionService::stopCurrentFileTransfer);
	connect(pWebViewWidget, &KWebViewWidget::requestApplicationSettingsRequested,
		m_pApplicationSettingsService, &KApplicationSettingsService::requestSettings);
	connect(pWebViewWidget, &KWebViewWidget::updateApplicationSettingsRequested,
		m_pApplicationSettingsService, &KApplicationSettingsService::updateSettings);
	connect(pWebViewWidget, &KWebViewWidget::updateApplicationThemeRequested,
		m_pApplicationSettingsService, &KApplicationSettingsService::updateTheme);
	connect(pWebViewWidget, &KWebViewWidget::respondIncomingAccessRequestRequested,
		m_pSessionService, &KSessionCoordinator::respondIncomingAccessRequest);
	connect(pWebViewWidget, &KWebViewWidget::respondPairingRequestRequested,
		m_pSessionService, &KSessionCoordinator::respondPairingRequest);
	connect(pWebViewWidget, &KWebViewWidget::requestTrustedDevicesRequested,
		m_pSessionService, &KSessionCoordinator::requestTrustedDevices);
	connect(pWebViewWidget, &KWebViewWidget::updateTrustedDeviceRequested,
		m_pSessionService, &KSessionCoordinator::updateTrustedDevice);
	connect(pWebViewWidget, &KWebViewWidget::revokeTrustedDeviceRequested,
		m_pSessionService, &KSessionCoordinator::revokeTrustedDevice);
	connect(pWebViewWidget, &KWebViewWidget::requestRePairDeviceRequested,
		m_pSessionService, &KSessionCoordinator::requestRePairDevice);
	connect(pWebViewWidget, &KWebViewWidget::disconnectSessionRequested,
		m_pSessionViewModel, &KSessionViewModel::disconnectSession);
	connect(pWebViewWidget, &KWebViewWidget::startStreamingRequested,
		m_pSessionViewModel, &KSessionViewModel::startStreaming);
	connect(pWebViewWidget, &KWebViewWidget::stopStreamingRequested,
		m_pSessionViewModel, &KSessionViewModel::stopStreaming);

	connect(m_pSessionViewModel, &KSessionViewModel::statusChanged,
		pWebViewWidget, &KWebViewWidget::sendStatusChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::errorOccurred,
		pWebViewWidget, &KWebViewWidget::sendCaptureError);
	connect(m_pSessionViewModel, &KSessionViewModel::sessionErrorOccurred,
		pWebViewWidget, &KWebViewWidget::sendSessionError);
	connect(m_pSessionViewModel, &KSessionViewModel::frameReady,
		pWebViewWidget, &KWebViewWidget::sendFrameReady);
	connect(m_pSessionViewModel, &KSessionViewModel::networkStatsReady,
		pWebViewWidget, &KWebViewWidget::sendNetworkStatsChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::signalingChanged,
		pWebViewWidget, &KWebViewWidget::sendSignalingChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::webRtcStateChanged,
		pWebViewWidget, &KWebViewWidget::sendWebRtcStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::sessionChannelChanged,
		pWebViewWidget, &KWebViewWidget::sendSessionChannelChanged);
	connect(m_pSessionService, &KSessionCoordinator::sessionCapabilitiesChanged,
		pWebViewWidget, &KWebViewWidget::sendSessionCapabilitiesChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWebViewWidget, &KWebViewWidget::sendDeviceInfoChanged);
	connect(m_pDiscoveryViewModel, &KDeviceDiscoveryViewModel::lanDevicesChanged,
		pWebViewWidget, &KWebViewWidget::sendLanDevicesChanged);
	connect(m_pDiscoveryViewModel, &KDeviceDiscoveryViewModel::lanDiscoveryError,
		pWebViewWidget, &KWebViewWidget::sendLanDiscoveryError);
	connect(m_pRecentDeviceService, &KRecentDeviceService::devicesChanged,
		pWebViewWidget, &KWebViewWidget::sendRecentDevicesChanged);
	connect(m_pRecentDeviceService, &KRecentDeviceService::recentDeviceError,
		pWebViewWidget, &KWebViewWidget::sendRecentDeviceError);
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::settingsChanged,
		pWebViewWidget, &KWebViewWidget::sendApplicationSettingsChanged);
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::settingsError,
		pWebViewWidget, &KWebViewWidget::sendApplicationSettingsError);
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::themeError,
		pWebViewWidget, &KWebViewWidget::sendApplicationThemeError);
	connect(m_pSessionService, &KSessionCoordinator::incomingAccessRequest,
		pWebViewWidget, &KWebViewWidget::sendIncomingAccessRequest);
	connect(m_pSessionService, &KSessionCoordinator::incomingAccessRequestCleared,
		pWebViewWidget, &KWebViewWidget::sendIncomingAccessRequestCleared);
	connect(m_pSessionService, &KSessionCoordinator::pairingRequested,
		pWebViewWidget, &KWebViewWidget::sendPairingRequest);
	connect(m_pSessionService, &KSessionCoordinator::pairingCleared,
		pWebViewWidget, &KWebViewWidget::sendPairingCleared);
	connect(m_pSessionService, &KSessionCoordinator::deviceAuthenticationStateChanged,
		pWebViewWidget, &KWebViewWidget::sendDeviceAuthenticationStateChanged);
	connect(m_pSessionService, &KSessionCoordinator::deviceAuthenticationStateChanged,
		m_pRecentDeviceService,
		[this](const QString &strState, const QString &strDeviceId,
			const QString &, bool)
		{
			if (strState == QStringLiteral("authenticated"))
				m_pRecentDeviceService->setAuthenticatedDeviceId(strDeviceId);
		});
	connect(m_pSessionService, &KSessionCoordinator::trustedDevicesChanged,
		pWebViewWidget, &KWebViewWidget::sendTrustedDevicesChanged);
	connect(m_pSessionService, &KSessionCoordinator::trustedDeviceError,
		pWebViewWidget, &KWebViewWidget::sendTrustedDeviceError);
	connect(m_pSessionService, &KSessionCoordinator::securityMigrationNotice,
		pWebViewWidget, &KWebViewWidget::sendSecurityMigrationNotice);
	connect(m_pSessionService, &KSessionCoordinator::sessionPermissionsChanged,
		pWebViewWidget, &KWebViewWidget::sendSessionPermissionsChanged);
	connect(m_pTerminalSessionService, &KTerminalSessionService::stateChanged,
		pWebViewWidget, &KWebViewWidget::sendTerminalStateChanged);
	connect(m_pTerminalSessionService, &KTerminalSessionService::incomingRequest,
		pWebViewWidget, &KWebViewWidget::sendIncomingTerminalRequest);
	connect(m_pTerminalSessionService, &KTerminalSessionService::incomingRequestCleared,
		pWebViewWidget, &KWebViewWidget::sendIncomingTerminalRequestCleared);
	connect(m_pTerminalSessionService, &KTerminalSessionService::structuredTerminalError,
		pWebViewWidget, [pWebViewWidget](const KSessionError &error)
		{
			pWebViewWidget->sendTerminalError(KSessionErrorPresenter::userMessage(error));
		});
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::stateChanged,
		pWebViewWidget,
		[this, pWebViewWidget](KFileTransferState state,
			bool bAvailable,
			const QString &strStatus)
		{
			m_strFileTransferStatus = strStatus;
			pWebViewWidget->sendFileTransferStateChanged(state, bAvailable,
				strStatus, m_strFileTransferDeviceName, QString(),
				m_nFileTransferActiveTaskCount,
				m_pSessionService->sessionGeneration());
		});
	connect(m_pFileTransferSessionService,
		&KFileTransferSessionService::controlledActivityChanged,
		pWebViewWidget,
		[this, pWebViewWidget](bool, int nActiveTaskCount)
		{
			m_nFileTransferActiveTaskCount = nActiveTaskCount;
			pWebViewWidget->sendFileTransferStateChanged(
				m_pFileTransferSessionService->state(),
				m_pFileTransferSessionService->isAvailable(),
				m_strFileTransferStatus,
				m_strFileTransferDeviceName,
				QString(), nActiveTaskCount,
				m_pSessionService->sessionGeneration());
		});
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::transferError,
		pWebViewWidget, &KWebViewWidget::sendFileTransferError);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWebViewWidget,
		[this, pWebViewWidget](const QString &strComputerName,
			const QString &, const QString &, int, int)
		{
			m_strFileTransferDeviceName = strComputerName;
			pWebViewWidget->sendFileTransferStateChanged(
				m_pFileTransferSessionService->state(),
				m_pFileTransferSessionService->isAvailable(),
				m_strFileTransferStatus, strComputerName, QString(),
				m_nFileTransferActiveTaskCount,
				m_pSessionService->sessionGeneration());
		});
	QString strTerminalSupportReason;
	const bool bTerminalFrontendSupported =
		m_pTerminalSessionService->isFrontendSupported(&strTerminalSupportReason);
	pWebViewWidget->sendTerminalFrontendSupportChanged(
		bTerminalFrontendSupported, strTerminalSupportReason);
	pWebViewWidget->sendFileTransferStateChanged(
		m_pFileTransferSessionService->state(),
		m_pFileTransferSessionService->isAvailable(),
		m_strFileTransferStatus,
		m_strFileTransferDeviceName,
		QString(), m_nFileTransferActiveTaskCount,
		m_pSessionService->sessionGeneration());
}

void KApplicationComposition::wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow)
{
	Q_ASSERT(pWindow != nullptr);
	const QSize remoteScreenSize = m_pSessionViewModel->remoteScreenSize();
	if (!remoteScreenSize.isEmpty())
		pWindow->setRemoteScreenSize(remoteScreenSize.width(), remoteScreenSize.height());
	pWindow->handleSessionCapabilitiesChanged(m_pSessionService->negotiatedCapabilities());
	pWindow->handlePrivacyModeStatusChanged(
		m_pDeviceSecurityPreferenceService->privacyModeStatus());
	pWindow->handlePostSessionActionStatusChanged(
		m_pDeviceSecurityPreferenceService->postSessionActionStatus());
	if (m_pDeviceSecurityPreferenceService->isPrivacyCommandPending())
		pWindow->handlePrivacyModeCommandStarted();
	if (m_pDeviceSecurityPreferenceService->isPostSessionActionCommandPending())
		pWindow->handlePostSessionActionCommandStarted();

	KWebViewWidget *pWebViewWidget = pWindow->webViewWidget();
	KVideoRenderWidget *pVideoRenderWidget = pWindow->videoRenderWidget();
	connect(pWebViewWidget, &KWebViewWidget::retryLastConnectionRequested,
		m_pSessionViewModel, &KSessionViewModel::retryLastConnection);
	connect(pWebViewWidget, &KWebViewWidget::setClipboardSyncEnabledRequested,
		m_pClipboardSyncService, &KClipboardSyncService::setEnabled);
	connect(pWebViewWidget, &KWebViewWidget::requestClipboardSyncStateRequested,
		m_pClipboardSyncService, &KClipboardSyncService::requestState);
	connect(m_pClipboardSyncService, &KClipboardSyncService::syncStateChanged,
		pWebViewWidget, &KWebViewWidget::sendClipboardSyncStateChanged);
	connect(m_pClipboardSyncService, &KClipboardSyncService::syncError,
		pWebViewWidget, &KWebViewWidget::sendClipboardSyncError);
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::settingsChanged,
		pWebViewWidget, &KWebViewWidget::sendApplicationSettingsChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::statusChanged,
		pWebViewWidget, &KWebViewWidget::sendStatusChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::errorOccurred,
		pWebViewWidget, &KWebViewWidget::sendCaptureError);
	connect(m_pSessionViewModel, &KSessionViewModel::sessionErrorOccurred,
		pWebViewWidget, &KWebViewWidget::sendSessionError);
	connect(m_pSessionViewModel, &KSessionViewModel::frameReady,
		pWebViewWidget, &KWebViewWidget::sendFrameReady);
	connect(m_pSessionViewModel, &KSessionViewModel::frameReady,
		pWindow, &KRemoteDesktopWindow::handleFrameReady);
	connect(m_pSessionViewModel, &KSessionViewModel::networkStatsReady,
		pWebViewWidget, &KWebViewWidget::sendNetworkStatsChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::signalingChanged,
		pWebViewWidget, &KWebViewWidget::sendSignalingChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::webRtcStateChanged,
		pWebViewWidget, &KWebViewWidget::sendWebRtcStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::sessionStateChanged,
		pWindow, &KRemoteDesktopWindow::handleSessionStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWebViewWidget, &KWebViewWidget::sendDeviceInfoChanged);
	connect(m_pSessionService, &KSessionCoordinator::sessionCapabilitiesChanged,
		pWebViewWidget, &KWebViewWidget::sendSessionCapabilitiesChanged);
	connect(m_pSessionService, &KSessionCoordinator::sessionCapabilitiesChanged,
		pWindow, &KRemoteDesktopWindow::handleSessionCapabilitiesChanged);
	connect(m_pSessionService, &KSessionCoordinator::privacyModeStatusChanged,
		pWindow, &KRemoteDesktopWindow::handlePrivacyModeStatusChanged);
	connect(m_pSessionService, &KSessionCoordinator::postSessionActionStatusChanged,
		pWindow, &KRemoteDesktopWindow::handlePostSessionActionStatusChanged);
	connect(m_pSessionService, &KSessionCoordinator::privacyModeCommandCompleted,
		pWindow, &KRemoteDesktopWindow::handlePrivacyModeCommandCompleted);
	connect(m_pSessionService, &KSessionCoordinator::postSessionActionCommandCompleted,
		pWindow, &KRemoteDesktopWindow::handlePostSessionActionCommandCompleted);
	connect(m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::privacyModeCommandStarted,
		pWindow, &KRemoteDesktopWindow::handlePrivacyModeCommandStarted);
	connect(m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::postSessionActionCommandStarted,
		pWindow, &KRemoteDesktopWindow::handlePostSessionActionCommandStarted);
	connect(m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::preferenceError,
		pWindow, &KRemoteDesktopWindow::handleSecurityPreferenceError);
	connect(pWindow, &KRemoteDesktopWindow::privacyModeRequested,
		m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::requestPrivacyMode);
	connect(pWindow, &KRemoteDesktopWindow::postSessionActionRequested,
		m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::requestPostSessionAction);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWindow,
		[pWindow](const QString &, const QString &, const QString &, int nScreenWidth, int nScreenHeight)
		{
			pWindow->setRemoteScreenSize(nScreenWidth, nScreenHeight);
		});
	connect(pWebViewWidget, &KWebViewWidget::streamConfigRequested,
		m_pSessionViewModel, &KSessionViewModel::sendStreamConfig);
	connect(pWindow, &KRemoteDesktopWindow::streamConfigRequested,
		m_pSessionViewModel, &KSessionViewModel::sendStreamConfig);
	connect(m_pSessionViewModel, &KSessionViewModel::renderFrameReady,
		pVideoRenderWidget, &KVideoRenderWidget::enqueueFrame);
	connect(m_pSessionViewModel, &KSessionViewModel::clearPreviewRequested,
		pVideoRenderWidget, &KVideoRenderWidget::clearFrame);
	connect(m_pSessionViewModel, &KSessionViewModel::suspendRemoteInputRequested,
		pVideoRenderWidget, &KVideoRenderWidget::suspendRemoteInput);
	connect(pVideoRenderWidget, &KVideoRenderWidget::remoteMouseMoveRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteMouseMove);
	connect(pVideoRenderWidget, &KVideoRenderWidget::remoteMouseButtonRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteMouseButton);
	connect(pVideoRenderWidget, &KVideoRenderWidget::remoteMouseWheelRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteMouseWheel);
	connect(pVideoRenderWidget, &KVideoRenderWidget::remoteKeyRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteKey);
	connect(pVideoRenderWidget, &KVideoRenderWidget::remoteTextRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteText);
	connect(pVideoRenderWidget, &KVideoRenderWidget::inputFeedbackRendered,
		m_pSessionViewModel, &KSessionViewModel::handleInputFeedbackRendered);
}

void KApplicationComposition::wireFileTransferWindow(KFileTransferWindow *pWindow)
{
	Q_ASSERT(pWindow != nullptr);
	KWebViewWidget *pWebViewWidget = pWindow->webViewWidget();
	Q_ASSERT(pWebViewWidget != nullptr);

	connect(pWebViewWidget, &KWebViewWidget::requestFileTransferSnapshotRequested,
		m_pFileTransferSessionService,
		&KFileTransferSessionService::requestSnapshot);
	connect(pWebViewWidget, &KWebViewWidget::stopCurrentFileTransferRequested,
		m_pFileTransferSessionService,
		&KFileTransferSessionService::stopCurrentFileTransfer);
	connect(pWindow, &KFileTransferWindow::fileTransferCloseRequested,
		m_pFileTransferSessionService,
		[this](bool bStopLogicalSession)
		{
			if (bStopLogicalSession)
				m_pFileTransferSessionService->stopCurrentFileTransfer();
		});
	connect(pWebViewWidget, &KWebViewWidget::navigateFilePaneRequested,
		m_pFileTransferSessionService,
		[this, pWebViewWidget](const QString &strPane,
			const QString &strListingId,
			const QString &strTargetEntryId)
		{
			KFileTransferPane pane;
			if (!ParseFileTransferPane(strPane, &pane))
			{
				pWebViewWidget->sendFileTransferError(
					QStringLiteral("invalid_pane"), QString());
				return;
			}
			m_pFileTransferSessionService->navigatePane(
				pane, strListingId, strTargetEntryId);
		});
	connect(pWebViewWidget, &KWebViewWidget::navigateFilePaneByPathRequested,
		m_pFileTransferSessionService,
		[this, pWebViewWidget](const QString &strPane, const QString &strPath)
		{
			KFileTransferPane pane;
			if (!ParseFileTransferPane(strPane, &pane))
			{
				pWebViewWidget->sendFileTransferError(
					QStringLiteral("invalid_pane"), QString());
				return;
			}
			m_pFileTransferSessionService->navigatePaneByPath(
				pane, strPath);
		});
	connect(pWebViewWidget, &KWebViewWidget::navigateFilePaneUpRequested,
		m_pFileTransferSessionService,
		[this, pWebViewWidget](const QString &strPane, const QString &strListingId)
		{
			KFileTransferPane pane;
			if (!ParseFileTransferPane(strPane, &pane))
			{
				pWebViewWidget->sendFileTransferError(
					QStringLiteral("invalid_pane"), QString());
				return;
			}
			m_pFileTransferSessionService->navigatePaneUp(
				pane, strListingId);
		});
	connect(pWebViewWidget, &KWebViewWidget::refreshFilePaneRequested,
		m_pFileTransferSessionService,
		[this, pWebViewWidget](const QString &strPane)
		{
			KFileTransferPane pane;
			if (!ParseFileTransferPane(strPane, &pane))
			{
				pWebViewWidget->sendFileTransferError(
					QStringLiteral("invalid_pane"), QString());
				return;
			}
			m_pFileTransferSessionService->refreshPane(pane);
		});
	connect(pWebViewWidget, &KWebViewWidget::startFileCopyRequested,
		m_pFileTransferSessionService,
		[this, pWebViewWidget](const QString &strSourcePane,
			const QString &strSourceListingId,
			const QStringList &sourceEntryIds,
			const QString &strDestinationListingId)
		{
			KFileTransferPane pane;
			if (!ParseFileTransferPane(strSourcePane, &pane))
			{
				pWebViewWidget->sendFileTransferError(
					QStringLiteral("invalid_pane"), QString());
				return;
			}
			m_pFileTransferSessionService->startCopy(
				pane,
				strSourceListingId, sourceEntryIds, strDestinationListingId);
		});
	connect(pWebViewWidget, &KWebViewWidget::pauseFileTransferTaskRequested,
		m_pFileTransferSessionService, &KFileTransferSessionService::pauseTask);
	connect(pWebViewWidget, &KWebViewWidget::resumeFileTransferTaskRequested,
		m_pFileTransferSessionService, &KFileTransferSessionService::resumeTask);
	connect(pWebViewWidget, &KWebViewWidget::cancelFileTransferTaskRequested,
		m_pFileTransferSessionService, &KFileTransferSessionService::cancelTask);
	connect(pWebViewWidget, &KWebViewWidget::retryFileTransferTaskRequested,
		m_pFileTransferSessionService, &KFileTransferSessionService::retryTask);
	connect(pWebViewWidget, &KWebViewWidget::resolveFileConflictRequested,
		m_pFileTransferSessionService,
		[this, pWebViewWidget](const QString &strConflictId,
			const QString &strResolution,
			bool bApplyToRemaining)
		{
			const KFileTransferConflictResolution resolution =
				FileTransferConflictResolutionFromName(strResolution);
			if (resolution == InvalidFileTransferConflictResolution)
			{
				pWebViewWidget->sendFileTransferError(
					QStringLiteral("invalid_conflict_resolution"),
					QStringLiteral("无法识别文件冲突处理方式"));
				return;
			}
			m_pFileTransferSessionService->resolveConflict(
				strConflictId, resolution, bApplyToRemaining);
		});
	connect(pWebViewWidget, &KWebViewWidget::clearCompletedFileTransferTasksRequested,
		m_pFileTransferSessionService,
		&KFileTransferSessionService::clearCompletedTasks);

	connect(m_pFileTransferSessionService, &KFileTransferSessionService::stateChanged,
		pWebViewWidget,
		[this, pWebViewWidget](KFileTransferState state,
			bool bAvailable,
			const QString &strStatus)
		{
			pWebViewWidget->sendFileTransferStateChanged(state, bAvailable,
				strStatus, m_strFileTransferDeviceName, QString(),
				m_nFileTransferActiveTaskCount,
				m_pSessionService->sessionGeneration());
		});
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::paneLoading,
		pWebViewWidget, &KWebViewWidget::sendFilePaneLoading);
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::paneChanged,
		pWebViewWidget, &KWebViewWidget::sendFilePaneChanged);
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::snapshotChanged,
		pWebViewWidget, &KWebViewWidget::sendFileTransferSnapshot);
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::taskChanged,
		pWebViewWidget, &KWebViewWidget::sendFileTransferTaskChanged);
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::taskRemoved,
		pWebViewWidget, &KWebViewWidget::sendFileTransferTaskRemoved);
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::conflictRequested,
		pWebViewWidget, &KWebViewWidget::sendFileTransferConflictRequested);
	connect(m_pFileTransferSessionService, &KFileTransferSessionService::transferError,
		pWebViewWidget, &KWebViewWidget::sendFileTransferError);
	connect(m_pFileTransferSessionService,
		&KFileTransferSessionService::controlledActivityChanged,
		pWindow, [pWindow](bool, int nActiveTaskCount)
		{
			pWindow->setHasActiveTasks(nActiveTaskCount > 0);
		});
	pWindow->setHasActiveTasks(m_pFileTransferSessionService->hasActiveTasks());

	pWebViewWidget->sendFileTransferStateChanged(
		m_pFileTransferSessionService->state(),
		m_pFileTransferSessionService->isAvailable(),
		m_strFileTransferStatus,
		m_strFileTransferDeviceName,
		QString(), m_nFileTransferActiveTaskCount,
		m_pSessionService->sessionGeneration());
	m_pFileTransferSessionService->requestSnapshot();
}

void KApplicationComposition::enterRemoteDesktop()
{
	m_pSessionViewModel->enterRemoteDesktop();
}

void KApplicationComposition::leaveRemoteDesktop()
{
	m_pSessionViewModel->leaveRemoteDesktop();
}

void KApplicationComposition::disconnectSession()
{
	m_pSessionViewModel->disconnectSession();
}

void KApplicationComposition::shutdown()
{
	if (m_bShutdown)
		return;

	m_bShutdown = true;
	m_pAutomationHostBridge->stopAcceptingRequests();
	m_spAutomationPluginLoader->shutdown();
	m_pShutdownDeadlineTimer->start();
	m_bSessionShutdownPending = !m_pSessionService->isIdle();
	m_bCaptureShutdownPending = true;
	m_bFileTransferShutdownPending = true;
	m_pClipboardSyncService->shutdown();
	m_pTerminalSessionService->shutdown();
	m_pFileTransferSessionService->shutdown();
	m_pDiscoveryService->stop();
	m_pSessionService->disconnectSession();
	m_pCaptureService->stopCapture();
	tryFinishShutdown();
}

void KApplicationComposition::tryFinishShutdown()
{
	if (!m_bShutdown || m_bShutdownFinished
		|| m_bSessionShutdownPending || m_bCaptureShutdownPending
		|| m_bFileTransferShutdownPending)
		return;
	m_bShutdownFinished = true;
	m_pShutdownDeadlineTimer->stop();
	emit shutdownFinished();
}

void KApplicationComposition::handleShutdownDeadline()
{
	if (!m_bShutdown || m_bShutdownFinished)
		return;

	m_bShutdownFinished = true;
	const QString strPending = QStringLiteral(
		"session=%1 capture=%2 fileTransfer=%3 deadlineMs=%4")
		.arg(m_bSessionShutdownPending ? 1 : 0)
		.arg(m_bCaptureShutdownPending ? 1 : 0)
		.arg(m_bFileTransferShutdownPending ? 1 : 0)
		.arg(kApplicationShutdownDeadlineMs);
	qCritical().noquote() << QStringLiteral("Application shutdown deadline reached: %1")
		.arg(strPending);
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("application_lifecycle"),
		QStringLiteral("shutdown_timeout"), -1, strPending);
	emit shutdownFinished();
}

void KApplicationComposition::wireServices()
{
	connect(m_pSessionService, &KSessionCoordinator::sessionStateChanged,
		this, [this](KSessionState state)
		{
			if (m_bShutdown && state == IdleSessionState)
			{
				m_bSessionShutdownPending = false;
				tryFinishShutdown();
			}
		});
	connect(m_pCaptureService, &KCaptureService::captureShutdownFinished,
		this, [this](quint64)
		{
			if (m_bShutdown)
			{
				m_bCaptureShutdownPending = false;
				tryFinishShutdown();
			}
		});
	connect(m_pFileTransferSessionService,
		&KFileTransferSessionService::shutdownFinished,
		this, [this]()
		{
			if (m_bShutdown)
			{
				m_bFileTransferShutdownPending = false;
				tryFinishShutdown();
			}
		});
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::settingsChanged,
		m_pSessionService, &KSessionCoordinator::applyApplicationSettings);
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::settingsChanged,
		this, [this](const KApplicationSettings &settings)
		{ m_pTerminalSessionService->setApprovalTimeoutSeconds(settings.nApprovalTimeoutSeconds); });
	connect(m_pSessionService, &KSessionCoordinator::trustedDeviceRevoked,
		m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::removePreference);
	connect(m_pDeviceSecurityPreferenceService,
		&KDeviceSecurityPreferenceService::preferenceError,
		this,
		[](const QString &strError)
		{
			qWarning().noquote() << QStringLiteral("Device security preference error: %1")
				.arg(strError);
		});
	connect(m_pCaptureService, &KCaptureService::webRtcFrameReady,
		m_pSessionService,
		[m_pSessionService = m_pSessionService](quint64 nGeneration, const KVideoFrame &frame)
		{
			if (m_pSessionService->sessionGeneration() == nGeneration)
				m_pSessionService->pushVideoFrame(frame);
		});
	connect(m_pCaptureService, &KCaptureService::captureError,
		m_pSessionService, &KSessionCoordinator::handleCaptureFailure);

	connect(m_pSessionService, &KSessionCoordinator::startCaptureRequested,
		m_pCaptureService, &KCaptureService::startWebRtcCapture);
	connect(m_pSessionService, &KSessionCoordinator::stopCaptureRequested,
		m_pCaptureService, &KCaptureService::requestStopCapture);
	connect(m_pCaptureService, &KCaptureService::captureShutdownFinished,
		m_pSessionService, &KSessionCoordinator::captureShutdownFinished);
	connect(m_pSessionService, &KSessionCoordinator::streamConfigChanged,
		m_pCaptureService, &KCaptureService::setStreamConfig);
	connect(m_pSessionService, &KSessionCoordinator::inputTraceUpdated,
		m_pCaptureService, &KCaptureService::setInputTraceState);
	connect(m_pSessionService, &KSessionCoordinator::inputFeedbackFrameRequested,
		m_pCaptureService, &KCaptureService::requestImmediateFrame);
	connect(m_pSessionService, &KSessionCoordinator::listeningAvailabilityChanged,
		m_pDiscoveryService, &KDeviceDiscoveryController::setListeningAvailability);
	connect(m_pDiscoveryService, &KDeviceDiscoveryController::connectEndpointRequested,
		m_pRecentDeviceService, &KRecentDeviceService::connectEndpoint);
	connect(m_pRecentDeviceService, &KRecentDeviceService::connectEndpointRequested,
		m_pSessionViewModel, &KSessionViewModel::connectSignaling);
	connect(m_pRecentDeviceService, &KRecentDeviceService::terminalEndpointRequested,
		m_pTerminalSessionService, &KTerminalSessionService::openTerminalForEndpoint);
	connect(m_pSessionViewModel, &KSessionViewModel::sessionChannelChanged,
		m_pRecentDeviceService, &KRecentDeviceService::setSessionChannelOpen);
	connect(m_pSessionService, &KSessionCoordinator::incomingAccessObserved,
		m_pRecentDeviceService,
		[this](const QString &strDeviceName, const QString &strSourceAddress)
		{
			m_pRecentDeviceService->prepareIncomingConnection(
				strDeviceName, strSourceAddress);
			m_pRecentDeviceService->setAuthenticatedDeviceId(
				m_pSessionService->authenticatedDeviceId());
		});
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		m_pRecentDeviceService,
		[m_pRecentDeviceService = m_pRecentDeviceService](const QString &strComputerName,
			const QString &, const QString &, int, int)
		{
			m_pRecentDeviceService->setRemoteDeviceName(strComputerName);
		});
}
