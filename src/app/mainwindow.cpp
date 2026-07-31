#include "app/mainwindow.h"

#include "app/remotedesktopwindow.h"
#include "render/videorenderwidget.h"
#include "session/sessionviewmodel.h"
#include "ui_bridge/webviewwidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtGui/QCloseEvent>

KMainWindow::KMainWindow(QWidget *pParent)
	: QMainWindow(pParent)
	, m_pSessionViewModel(new KSessionViewModel(this))
	, m_pWebViewWidget(new KWebViewWidget(this))
{
	setWindowTitle(QStringLiteral("winRemoteControl Preview"));
	setCentralWidget(m_pWebViewWidget);

	initConnections();

	m_strFrontendPath = QDir(QCoreApplication::applicationDirPath())
		.filePath(QStringLiteral("frontend/index.html"));
	m_pWebViewWidget->loadLocalFile(m_strFrontendPath, QStringLiteral("dashboard"));
}

KMainWindow::~KMainWindow()
{
}

void KMainWindow::closeEvent(QCloseEvent *pEvent)
{
	closeRemoteDesktopWindow();
	m_pSessionViewModel->disconnectSession();
	QMainWindow::closeEvent(pEvent);
}

void KMainWindow::initConnections()
{
	connect(m_pWebViewWidget, &KWebViewWidget::startCaptureRequested,
		m_pSessionViewModel, &KSessionViewModel::startLocalPreview);
	connect(m_pWebViewWidget, &KWebViewWidget::stopCaptureRequested,
		m_pSessionViewModel, &KSessionViewModel::stopCapture);
	connect(m_pWebViewWidget, &KWebViewWidget::setRoleRequested,
		m_pSessionViewModel, &KSessionViewModel::setRole);
	connect(m_pWebViewWidget, &KWebViewWidget::startSignalingServerRequested,
		m_pSessionViewModel, &KSessionViewModel::startSignalingServer);
	connect(m_pWebViewWidget, &KWebViewWidget::connectSignalingRequested,
		m_pSessionViewModel, &KSessionViewModel::connectSignaling);
	connect(m_pWebViewWidget, &KWebViewWidget::disconnectSessionRequested,
		m_pSessionViewModel, &KSessionViewModel::disconnectSession);
	connect(m_pWebViewWidget, &KWebViewWidget::startStreamingRequested,
		m_pSessionViewModel, &KSessionViewModel::startStreaming);
	connect(m_pWebViewWidget, &KWebViewWidget::stopStreamingRequested,
		m_pSessionViewModel, &KSessionViewModel::stopStreaming);
	connect(m_pWebViewWidget, &KWebViewWidget::enterDesktopRequested,
		this, &KMainWindow::openRemoteDesktopWindow);

	connect(m_pSessionViewModel, &KSessionViewModel::statusChanged,
		m_pWebViewWidget, &KWebViewWidget::sendStatusChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::errorOccurred,
		m_pWebViewWidget, &KWebViewWidget::sendCaptureError);
	connect(m_pSessionViewModel, &KSessionViewModel::frameReady,
		m_pWebViewWidget, &KWebViewWidget::sendFrameReady);
	connect(m_pSessionViewModel, &KSessionViewModel::networkStatsReady,
		m_pWebViewWidget, &KWebViewWidget::sendNetworkStatsChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::signalingChanged,
		m_pWebViewWidget, &KWebViewWidget::sendSignalingChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::webRtcStateChanged,
		m_pWebViewWidget, &KWebViewWidget::sendWebRtcStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::sessionChannelChanged,
		m_pWebViewWidget, &KWebViewWidget::sendSessionChannelChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		m_pWebViewWidget, &KWebViewWidget::sendDeviceInfoChanged);
}

void KMainWindow::openRemoteDesktopWindow()
{
	if (m_pRemoteDesktopWindow == nullptr)
	{
		m_pRemoteDesktopWindow = new KRemoteDesktopWindow(this);
		wireRemoteDesktopWindow(m_pRemoteDesktopWindow);
		m_pRemoteDesktopWindow->loadFrontend(m_strFrontendPath);
	}

	m_pRemoteDesktopWindow->show();
	m_pRemoteDesktopWindow->raise();
	m_pRemoteDesktopWindow->activateWindow();
	m_pSessionViewModel->enterRemoteDesktop();
}

void KMainWindow::closeRemoteDesktopWindow()
{
	if (m_pRemoteDesktopWindow == nullptr)
		return;

	m_pSessionViewModel->leaveRemoteDesktop();
	m_pRemoteDesktopWindow->deleteLater();
	m_pRemoteDesktopWindow = nullptr;
}

void KMainWindow::wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow)
{
	const QSize remoteScreenSize = m_pSessionViewModel->remoteScreenSize();
	if (!remoteScreenSize.isEmpty())
		pWindow->setRemoteScreenSize(remoteScreenSize.width(), remoteScreenSize.height());

	connect(pWindow, &KRemoteDesktopWindow::desktopCloseRequested,
		this, &KMainWindow::closeRemoteDesktopWindow);
	connect(m_pSessionViewModel, &KSessionViewModel::statusChanged,
		pWindow->webViewWidget(), &KWebViewWidget::sendStatusChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::errorOccurred,
		pWindow->webViewWidget(), &KWebViewWidget::sendCaptureError);
	connect(m_pSessionViewModel, &KSessionViewModel::frameReady,
		pWindow->webViewWidget(), &KWebViewWidget::sendFrameReady);
	connect(m_pSessionViewModel, &KSessionViewModel::frameReady,
		pWindow, &KRemoteDesktopWindow::handleFrameReady);
	connect(m_pSessionViewModel, &KSessionViewModel::networkStatsReady,
		pWindow->webViewWidget(), &KWebViewWidget::sendNetworkStatsChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::signalingChanged,
		pWindow->webViewWidget(), &KWebViewWidget::sendSignalingChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::webRtcStateChanged,
		pWindow->webViewWidget(), &KWebViewWidget::sendWebRtcStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::webRtcStateChanged,
		pWindow, &KRemoteDesktopWindow::handleSessionStateChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWindow->webViewWidget(), &KWebViewWidget::sendDeviceInfoChanged);
	connect(m_pSessionViewModel, &KSessionViewModel::remoteDeviceInfoChanged,
		pWindow,
		[pWindow](const QString &, const QString &, const QString &, int nScreenWidth, int nScreenHeight)
		{
			pWindow->setRemoteScreenSize(nScreenWidth, nScreenHeight);
		});
	connect(pWindow->webViewWidget(), &KWebViewWidget::streamConfigRequested,
		m_pSessionViewModel, &KSessionViewModel::sendStreamConfig);
	connect(pWindow, &KRemoteDesktopWindow::streamConfigRequested,
		m_pSessionViewModel, &KSessionViewModel::sendStreamConfig);
	connect(m_pSessionViewModel, &KSessionViewModel::renderFrameReady,
		pWindow->videoRenderWidget(), &KVideoRenderWidget::enqueueFrame);
	connect(m_pSessionViewModel, &KSessionViewModel::clearPreviewRequested,
		pWindow->videoRenderWidget(), &KVideoRenderWidget::clearFrame);
	connect(pWindow->videoRenderWidget(), &KVideoRenderWidget::remoteMouseMoveRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteMouseMove);
	connect(pWindow->videoRenderWidget(), &KVideoRenderWidget::remoteMouseButtonRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteMouseButton);
	connect(pWindow->videoRenderWidget(), &KVideoRenderWidget::remoteMouseWheelRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteMouseWheel);
	connect(pWindow->videoRenderWidget(), &KVideoRenderWidget::remoteKeyRequested,
		m_pSessionViewModel, &KSessionViewModel::sendRemoteKey);
	connect(pWindow->videoRenderWidget(), &KVideoRenderWidget::inputFeedbackRendered,
		m_pSessionViewModel, &KSessionViewModel::handleInputFeedbackRendered);
}
