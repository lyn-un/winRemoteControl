#include "app/remotedesktopwindow.h"

#include "render/videorenderwidget.h"
#include "ui_bridge/webviewwidget.h"

#include <QtCore/QtMath>
#include <QtGui/QCloseEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QMenu>

#include <Windows.h>
#include <windowsx.h>

namespace
{
	constexpr int kUltraFastWidth = 1280;
	constexpr int kUltraFastHeight = 720;
	constexpr int kUltraFastFps = 60;
	constexpr int kUltraFastBitrateKbps = 4000;
	constexpr int kAutoWidth = 1280;
	constexpr int kAutoHeight = 720;
	constexpr int kAutoFps = 30;
	constexpr int kAutoBitrateKbps = 3000;
	constexpr int kOriginalWidth = 0;
	constexpr int kOriginalHeight = 0;
	constexpr int kOriginalFps = 30;
	constexpr int kOriginalBitrateKbps = 12000;
	constexpr int kHdWidth = 1920;
	constexpr int kHdHeight = 1080;
	constexpr int kHdFps = 30;
	constexpr int kHdBitrateKbps = 6000;
	constexpr int kSmoothWidth = 1280;
	constexpr int kSmoothHeight = 720;
	constexpr int kSmoothFps = 30;
	constexpr int kSmoothBitrateKbps = 2000;
	constexpr double kMaxInitialWindowScale = 0.9;
	constexpr int kMinimumWindowWidth = 640;
	constexpr int kMinimumWindowHeight = 420;
	constexpr int kDesktopTitleBarHeight = 40;
	constexpr int kResizeBorderDip = 8;
}

KRemoteDesktopWindow::KRemoteDesktopWindow(QWidget *pParent)
	: QMainWindow(pParent)
	, m_pWebViewWidget(new KWebViewWidget(this))
	, m_pVideoRenderWidget(new KVideoRenderWidget(m_pWebViewWidget))
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
	setWindowTitle(QStringLiteral("winRemoteControl Desktop"));
	resize(1280, 760);
	m_pVideoRenderWidget->hide();
	setCentralWidget(m_pWebViewWidget);
	initConnections();
}

KRemoteDesktopWindow::~KRemoteDesktopWindow()
{
}

KWebViewWidget *KRemoteDesktopWindow::webViewWidget() const
{
	return m_pWebViewWidget;
}

KVideoRenderWidget *KRemoteDesktopWindow::videoRenderWidget() const
{
	return m_pVideoRenderWidget;
}

void KRemoteDesktopWindow::setRemoteScreenSize(int nWidth, int nHeight)
{
	m_pVideoRenderWidget->setRemoteScreenSize(nWidth, nHeight);
}

void KRemoteDesktopWindow::handleFrameReady(int nWidth,
	int nHeight,
	quint64 nFrameIndex,
	qint64 nTimestampMs)
{
	Q_UNUSED(nFrameIndex)
	Q_UNUSED(nTimestampMs)

	adjustInitialWindowSize(nWidth, nHeight);
}

void KRemoteDesktopWindow::handleSessionStateChanged(KSessionState state)
{
	const bool bAvailable = state == ConnectedSessionState || state == StreamingSessionState;
	m_bSessionAvailable = bAvailable;
	if (!m_bSessionAvailable)
	{
		m_pVideoRenderWidget->hide();
		return;
	}

	if (m_previewRect.width() > 2 && m_previewRect.height() > 2)
	{
		m_pVideoRenderWidget->setGeometry(m_previewRect);
		m_pVideoRenderWidget->show();
		m_pVideoRenderWidget->raise();
	}
}

void KRemoteDesktopWindow::loadFrontend(const QString &strFrontendPath)
{
	m_pWebViewWidget->loadLocalFile(strFrontendPath, QStringLiteral("desktop"));
}

void KRemoteDesktopWindow::closeEvent(QCloseEvent *pEvent)
{
	if (!m_bClosing)
	{
		m_bClosing = true;
		emit desktopCloseRequested();
	}
	QMainWindow::closeEvent(pEvent);
}

bool KRemoteDesktopWindow::nativeEvent(const QByteArray &eventType, void *pMessage, qintptr *pResult)
{
	if (eventType == QByteArrayLiteral("windows_generic_MSG")
		&& handleNativeHitTest(pMessage, pResult))
	{
		return true;
	}

	return QMainWindow::nativeEvent(eventType, pMessage, pResult);
}

void KRemoteDesktopWindow::initConnections()
{
	connect(m_pWebViewWidget, &KWebViewWidget::previewRectChanged,
		this, &KRemoteDesktopWindow::updatePreviewRect);
	connect(m_pWebViewWidget, &KWebViewWidget::closeDesktopRequested,
		this, &KRemoteDesktopWindow::close);
	connect(m_pWebViewWidget, &KWebViewWidget::minimizeDesktopWindowRequested,
		this, &KRemoteDesktopWindow::minimizeWindow);
	connect(m_pWebViewWidget, &KWebViewWidget::toggleMaximizeDesktopWindowRequested,
		this, &KRemoteDesktopWindow::toggleMaximizeWindow);
	connect(m_pWebViewWidget, &KWebViewWidget::beginDesktopWindowDragRequested,
		this, &KRemoteDesktopWindow::beginWindowDrag);
	connect(m_pWebViewWidget, &KWebViewWidget::showControlCenterMenuRequested,
		this, &KRemoteDesktopWindow::showControlCenterMenu);
	connect(m_pVideoRenderWidget, &KVideoRenderWidget::renderError,
		m_pWebViewWidget, &KWebViewWidget::sendCaptureError);
}

void KRemoteDesktopWindow::updatePreviewRect(const QRect &rect)
{
	m_previewRect = rect;
	if (!m_bSessionAvailable || rect.width() <= 2 || rect.height() <= 2)
	{
		m_pVideoRenderWidget->hide();
		return;
	}

	m_pVideoRenderWidget->setGeometry(rect);
	m_pVideoRenderWidget->show();
	m_pVideoRenderWidget->raise();
}

void KRemoteDesktopWindow::showControlCenterMenu(const QPoint &pos)
{
	QMenu menu(this);
	menu.setStyleSheet(QStringLiteral(
		"QMenu { background: #f7fbfc; color: #25343a; border: 1px solid #c7d9dd; "
		"border-radius: 7px; padding: 6px; }"
		"QMenu::item { min-width: 150px; padding: 8px 24px 8px 12px; border-radius: 5px; }"
		"QMenu::item:selected { background: #dce8eb; color: #315f6b; }"
		"QMenu::separator { height: 1px; background: #cfdee2; margin: 5px 8px; }"));
	QMenu *pQualityMenu = menu.addMenu(QStringLiteral("画质"));
	pQualityMenu->addAction(QStringLiteral("极速"), this,
		[this]()
		{
			applyStreamConfig(kUltraFastWidth, kUltraFastHeight, kUltraFastFps, kUltraFastBitrateKbps);
		});
	pQualityMenu->addAction(QStringLiteral("自动"), this,
		[this]()
		{
			applyStreamConfig(kAutoWidth, kAutoHeight, kAutoFps, kAutoBitrateKbps);
		});
	pQualityMenu->addAction(QStringLiteral("原画"), this,
		[this]()
		{
			applyStreamConfig(kOriginalWidth, kOriginalHeight, kOriginalFps, kOriginalBitrateKbps);
		});
	pQualityMenu->addAction(QStringLiteral("高清"), this,
		[this]()
		{
			applyStreamConfig(kHdWidth, kHdHeight, kHdFps, kHdBitrateKbps);
		});
	pQualityMenu->addAction(QStringLiteral("流畅"), this,
		[this]()
		{
			applyStreamConfig(kSmoothWidth, kSmoothHeight, kSmoothFps, kSmoothBitrateKbps);
		});

	menu.exec(m_pWebViewWidget->mapToGlobal(pos));
}

void KRemoteDesktopWindow::applyStreamConfig(int nWidth, int nHeight, int nFps, int nBitrateKbps)
{
	m_streamConfig.nWidth = nWidth;
	m_streamConfig.nHeight = nHeight;
	m_streamConfig.nFps = nFps;
	m_streamConfig.nBitrateKbps = nBitrateKbps;
	emit streamConfigRequested(m_streamConfig);
}

void KRemoteDesktopWindow::minimizeWindow()
{
	showMinimized();
}

void KRemoteDesktopWindow::toggleMaximizeWindow()
{
	if (isMaximized())
		showNormal();
	else
		showMaximized();
}

void KRemoteDesktopWindow::beginWindowDrag()
{
	::ReleaseCapture();
	::SendMessageW(reinterpret_cast<HWND>(winId()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void KRemoteDesktopWindow::adjustInitialWindowSize(int nFrameWidth, int nFrameHeight)
{
	if (m_bInitialSizeAdjusted || nFrameWidth <= 0 || nFrameHeight <= 0 || isMaximized())
		return;

	QScreen *pScreen = screen();
	if (pScreen == nullptr)
		pScreen = QGuiApplication::primaryScreen();
	if (pScreen == nullptr)
		return;

	const QRect availableRect = pScreen->availableGeometry();
	const int nMaxWindowWidth = qMax(kMinimumWindowWidth, qFloor(availableRect.width() * kMaxInitialWindowScale));
	const int nMaxWindowHeight = qMax(kMinimumWindowHeight, qFloor(availableRect.height() * kMaxInitialWindowScale));
	const int nMaxVideoHeight = qMax(1, nMaxWindowHeight - kDesktopTitleBarHeight);
	const double fFrameAspect = static_cast<double>(nFrameWidth) / static_cast<double>(nFrameHeight);

	int nVideoWidth = nMaxWindowWidth;
	int nVideoHeight = qRound(nVideoWidth / fFrameAspect);
	if (nVideoHeight > nMaxVideoHeight)
	{
		nVideoHeight = nMaxVideoHeight;
		nVideoWidth = qRound(nVideoHeight * fFrameAspect);
	}

	const QSize targetSize(qMax(kMinimumWindowWidth, nVideoWidth),
		qMax(kMinimumWindowHeight, nVideoHeight + kDesktopTitleBarHeight));
	resize(targetSize);
	move(availableRect.center() - rect().center());
	m_bInitialSizeAdjusted = true;
}

bool KRemoteDesktopWindow::handleNativeHitTest(void *pMessage, qintptr *pResult) const
{
	MSG *pMsg = static_cast<MSG *>(pMessage);
	if (pMsg == nullptr || pMsg->message != WM_NCHITTEST || pResult == nullptr)
		return false;
	if (isMaximized())
		return false;

	const int nBorderWidth = static_cast<int>(kResizeBorderDip * devicePixelRatioF());
	const LONG nX = GET_X_LPARAM(pMsg->lParam);
	const LONG nY = GET_Y_LPARAM(pMsg->lParam);
	RECT windowRect = {};
	if (!::GetWindowRect(reinterpret_cast<HWND>(winId()), &windowRect))
		return false;

	const bool bLeft = nX >= windowRect.left && nX < windowRect.left + nBorderWidth;
	const bool bRight = nX <= windowRect.right && nX > windowRect.right - nBorderWidth;
	const bool bTop = nY >= windowRect.top && nY < windowRect.top + nBorderWidth;
	const bool bBottom = nY <= windowRect.bottom && nY > windowRect.bottom - nBorderWidth;

	if (bLeft && bTop)
		*pResult = HTTOPLEFT;
	else if (bRight && bTop)
		*pResult = HTTOPRIGHT;
	else if (bLeft && bBottom)
		*pResult = HTBOTTOMLEFT;
	else if (bRight && bBottom)
		*pResult = HTBOTTOMRIGHT;
	else if (bLeft)
		*pResult = HTLEFT;
	else if (bRight)
		*pResult = HTRIGHT;
	else if (bTop)
		*pResult = HTTOP;
	else if (bBottom)
		*pResult = HTBOTTOM;
	else
		return false;

	return true;
}
