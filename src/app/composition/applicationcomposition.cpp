#include "app/composition/applicationcomposition.h"

#include "adapters/windows/device/windowsdeviceinfoprovider.h"
#include "adapters/windows/input/windowsinputinjector.h"
#include "adapters/clipboard/qtclipboardadapter.h"
#include "adapters/signaling/tcpsignalingtransport.h"
#include "adapters/discovery/udplandiscoverytransport.h"
#include "adapters/settings/qsettingsrecentdevicestore.h"
#include "adapters/settings/qsettingsapplicationstore.h"
#include "app/remotedesktopwindow.h"
#include "capture/captureservice.h"
#include "core/discovery/devicediscoverycontroller.h"
#include "discovery/landiscoveryservice.h"
#include "devices/recentdeviceservice.h"
#include "render/videorenderwidget.h"
#include "session/sessionviewmodel.h"
#include "transport/webrtc/webrtcpeer.h"
#include "session/sessioncoordinator.h"
#include "settings/applicationsettingsservice.h"
#include "clipboard/clipboardsyncservice.h"
#include "ui_bridge/webviewwidget.h"
#include "ui_bridge/devicediscoveryviewmodel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSysInfo>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <memory>

namespace
{
	QString RecentDevicesFilePath()
	{
		const QString strFileName = QStringLiteral("recent_devices.ini");
		const QString strFilePath = QDir(QCoreApplication::applicationDirPath()).filePath(strFileName);
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
		return QDir(QCoreApplication::applicationDirPath())
			.filePath(QStringLiteral("settings.ini"));
	}
}

KApplicationComposition::KApplicationComposition(QObject *pParent)
	: QObject(pParent)
	, m_pCaptureService(new KCaptureService(this))
	, m_pSessionService(new KSessionCoordinator(
		std::make_unique<KWindowsDeviceInfoProvider>(),
		std::make_unique<KWindowsInputInjector>(),
		std::make_unique<KWebRtcPeer>(),
		std::make_unique<KTcpSignalingTransport>(),
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
	, m_pClipboardSyncService(new KClipboardSyncService(
		std::make_unique<KQtClipboardAdapter>(),
		m_pSessionService,
		this))
{
	m_pRecentDeviceService->initialize();
	m_pApplicationSettingsService->initialize();
	m_pSessionService->applyApplicationSettings(m_pApplicationSettingsService->settings());
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

void KApplicationComposition::wireDashboard(KWebViewWidget *pWebViewWidget)
{
	Q_ASSERT(pWebViewWidget != nullptr);
	connect(pWebViewWidget, &KWebViewWidget::startCaptureRequested,
		m_pSessionViewModel, &KSessionViewModel::startLocalPreview);
	connect(pWebViewWidget, &KWebViewWidget::stopCaptureRequested,
		m_pSessionViewModel, &KSessionViewModel::stopCapture);
	connect(pWebViewWidget, &KWebViewWidget::setRoleRequested,
		m_pSessionViewModel, &KSessionViewModel::setRole);
	connect(pWebViewWidget, &KWebViewWidget::setRoleRequested,
		m_pDiscoveryViewModel, &KDeviceDiscoveryViewModel::setRole);
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
	connect(pWebViewWidget, &KWebViewWidget::requestApplicationSettingsRequested,
		m_pApplicationSettingsService, &KApplicationSettingsService::requestSettings);
	connect(pWebViewWidget, &KWebViewWidget::updateApplicationSettingsRequested,
		m_pApplicationSettingsService, &KApplicationSettingsService::updateSettings);
	connect(pWebViewWidget, &KWebViewWidget::respondIncomingAccessRequestRequested,
		m_pSessionService, &KSessionCoordinator::respondIncomingAccessRequest);
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
	connect(m_pSessionService, &KSessionCoordinator::incomingAccessRequest,
		pWebViewWidget, &KWebViewWidget::sendIncomingAccessRequest);
	connect(m_pSessionService, &KSessionCoordinator::incomingAccessRequestCleared,
		pWebViewWidget, &KWebViewWidget::sendIncomingAccessRequestCleared);
}

void KApplicationComposition::wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow)
{
	Q_ASSERT(pWindow != nullptr);
	const QSize remoteScreenSize = m_pSessionViewModel->remoteScreenSize();
	if (!remoteScreenSize.isEmpty())
		pWindow->setRemoteScreenSize(remoteScreenSize.width(), remoteScreenSize.height());

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
	connect(m_pSessionViewModel, &KSessionViewModel::statusChanged,
		pWebViewWidget, &KWebViewWidget::sendStatusChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::errorOccurred,
		pWebViewWidget, &KWebViewWidget::sendCaptureError);
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
	connect(m_pSessionViewModel, &KSessionViewModel::webRtcStateChanged,
		pWindow, &KRemoteDesktopWindow::handleSessionStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWebViewWidget, &KWebViewWidget::sendDeviceInfoChanged);
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
	connect(pVideoRenderWidget, &KVideoRenderWidget::inputFeedbackRendered,
		m_pSessionViewModel, &KSessionViewModel::handleInputFeedbackRendered);
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
	m_pClipboardSyncService->shutdown();
	m_pDiscoveryService->stop();
	m_pSessionService->disconnectSession();
	m_pCaptureService->stopCapture();
}

void KApplicationComposition::wireServices()
{
	connect(m_pApplicationSettingsService, &KApplicationSettingsService::settingsChanged,
		m_pSessionService, &KSessionCoordinator::applyApplicationSettings);
	connect(m_pCaptureService, &KCaptureService::webRtcFrameReady,
		m_pSessionService, &KSessionCoordinator::pushVideoFrame);
	connect(m_pCaptureService, &KCaptureService::captureError,
		m_pSessionService, &KSessionCoordinator::handleCaptureFailure);

	connect(m_pSessionService, &KSessionCoordinator::startCaptureRequested,
		m_pCaptureService, &KCaptureService::startWebRtcCapture);
	connect(m_pSessionService, &KSessionCoordinator::stopCaptureRequested,
		m_pCaptureService, &KCaptureService::stopCapture);
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
	connect(m_pSessionViewModel, &KSessionViewModel::sessionChannelChanged,
		m_pRecentDeviceService, &KRecentDeviceService::setSessionChannelOpen);
	connect(m_pSessionService, &KSessionCoordinator::incomingAccessObserved,
		m_pRecentDeviceService, &KRecentDeviceService::prepareIncomingConnection);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		m_pRecentDeviceService,
		[m_pRecentDeviceService = m_pRecentDeviceService](const QString &strComputerName,
			const QString &, const QString &, int, int)
		{
			m_pRecentDeviceService->setRemoteDeviceName(strComputerName);
		});
}
