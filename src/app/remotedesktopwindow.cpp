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
	if (rect.width() <= 2 || rect.height() <= 2)
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
	QMenu *pQualityMenu = menu.addMenu(QStringLiteral("画质"));
	pQualityMenu->addAction(QStringLiteral("自动"), this,
		[this]()
		{
			applyStreamQuality(1280, 720, 2000);
		});
	pQualityMenu->addAction(QStringLiteral("原画"), this,
		[this]()
		{
			applyStreamQuality(0, 0, 20000);
		});
	pQualityMenu->addAction(QStringLiteral("高清"), this,
		[this]()
		{
			applyStreamQuality(1920, 1080, 8000);
		});
	pQualityMenu->addAction(QStringLiteral("流畅"), this,
		[this]()
		{
			applyStreamQuality(1280, 720, 2000);
		});
	pQualityMenu->addSeparator();
	pQualityMenu->addAction(QStringLiteral("60 帧"), this,
		[this]()
		{
			applyStreamFps(60);
		});
	pQualityMenu->addAction(QStringLiteral("30 帧"), this,
		[this]()
		{
			applyStreamFps(30);
		});

	menu.exec(m_pWebViewWidget->mapToGlobal(pos));
}

void KRemoteDesktopWindow::applyStreamQuality(int nWidth, int nHeight, int nBitrateKbps)
{
	m_streamConfig.nWidth = nWidth;
	m_streamConfig.nHeight = nHeight;
	m_streamConfig.nBitrateKbps = nBitrateKbps;
	emit streamConfigRequested(m_streamConfig);
}

void KRemoteDesktopWindow::applyStreamFps(int nFps)
{
	m_streamConfig.nFps = nFps;
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
	const int nMaxWindowWidth = qMax(640, qFloor(availableRect.width() * 0.9));
	const int nMaxWindowHeight = qMax(420, qFloor(availableRect.height() * 0.9));
	const int nTitleBarHeight = 40;
	const int nMaxVideoHeight = qMax(1, nMaxWindowHeight - nTitleBarHeight);
	const double fFrameAspect = static_cast<double>(nFrameWidth) / static_cast<double>(nFrameHeight);

	int nVideoWidth = nMaxWindowWidth;
	int nVideoHeight = qRound(nVideoWidth / fFrameAspect);
	if (nVideoHeight > nMaxVideoHeight)
	{
		nVideoHeight = nMaxVideoHeight;
		nVideoWidth = qRound(nVideoHeight * fFrameAspect);
	}

	const QSize targetSize(qMax(640, nVideoWidth), qMax(420, nVideoHeight + nTitleBarHeight));
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

	const int nBorderWidth = static_cast<int>(8 * devicePixelRatioF());
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
