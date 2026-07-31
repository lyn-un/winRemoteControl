#include "app/composition/applicationcomposition.h"

#include "adapters/windows/device/windowsdeviceinfoprovider.h"
#include "adapters/windows/input/windowsinputinjector.h"
#include "app/remotedesktopwindow.h"
#include "capture/captureservice.h"
#include "render/videorenderwidget.h"
#include "session/sessionviewmodel.h"
#include "transport/webrtc/webrtcpeer.h"
#include "transport/webrtc/webrtcsessionservice.h"
#include "ui_bridge/webviewwidget.h"

#include <memory>

KApplicationComposition::KApplicationComposition(QObject *pParent)
	: QObject(pParent)
	, m_pCaptureService(new KCaptureService(this))
	, m_pSessionService(new KWebRtcSessionService(
		std::make_unique<KWindowsDeviceInfoProvider>(),
		std::make_unique<KWindowsInputInjector>(),
		std::make_unique<KWebRtcPeer>(),
		this))
	, m_pSessionViewModel(new KSessionViewModel(
		m_pCaptureService,
		m_pSessionService,
		this))
{
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
	connect(pWebViewWidget, &KWebViewWidget::startSignalingServerRequested,
		m_pSessionViewModel, &KSessionViewModel::startSignalingServer);
	connect(pWebViewWidget, &KWebViewWidget::connectSignalingRequested,
		m_pSessionViewModel, &KSessionViewModel::connectSignaling);
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
}

void KApplicationComposition::wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow)
{
	Q_ASSERT(pWindow != nullptr);
	const QSize remoteScreenSize = m_pSessionViewModel->remoteScreenSize();
	if (!remoteScreenSize.isEmpty())
		pWindow->setRemoteScreenSize(remoteScreenSize.width(), remoteScreenSize.height());

	KWebViewWidget *pWebViewWidget = pWindow->webViewWidget();
	KVideoRenderWidget *pVideoRenderWidget = pWindow->videoRenderWidget();
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
	m_pSessionService->disconnectSession();
	m_pCaptureService->stopCapture();
}

void KApplicationComposition::wireServices()
{
	connect(m_pCaptureService, &KCaptureService::webRtcFrameReady,
		m_pSessionService, &KWebRtcSessionService::pushVideoFrame);
	connect(m_pCaptureService, &KCaptureService::captureError,
		m_pSessionService, &KWebRtcSessionService::handleCaptureFailure);

	connect(m_pSessionService, &KWebRtcSessionService::startCaptureRequested,
		m_pCaptureService, &KCaptureService::startWebRtcCapture);
	connect(m_pSessionService, &KWebRtcSessionService::stopCaptureRequested,
		m_pCaptureService, &KCaptureService::stopCapture);
	connect(m_pSessionService, &KWebRtcSessionService::streamConfigChanged,
		m_pCaptureService, &KCaptureService::setStreamConfig);
	connect(m_pSessionService, &KWebRtcSessionService::inputTraceUpdated,
		m_pCaptureService, &KCaptureService::setInputTraceState);
	connect(m_pSessionService, &KWebRtcSessionService::inputFeedbackFrameRequested,
		m_pCaptureService, &KCaptureService::requestImmediateFrame);
}
