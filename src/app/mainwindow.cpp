#include "app/mainwindow.h"

#include "app/composition/applicationcomposition.h"
#include "app/remotedesktopwindow.h"
#include "ui_bridge/webviewwidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtGui/QCloseEvent>

#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>

namespace
{
	constexpr int kResizeBorderDip = 8;
}

KMainWindow::KMainWindow(QWidget *pParent)
	: QMainWindow(pParent)
	, m_pComposition(new KApplicationComposition(this))
	, m_pWebViewWidget(new KWebViewWidget(this))
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
	setWindowTitle(QStringLiteral("winRemoteControl Preview"));
	setCentralWidget(m_pWebViewWidget);
	applyWindowCorners();

	initConnections();
	connect(m_pComposition, &KApplicationComposition::shutdownFinished,
		this, [this]()
		{
			m_bShutdownComplete = true;
			close();
		});

	m_strFrontendPath = QDir(QCoreApplication::applicationDirPath())
		.filePath(QStringLiteral("frontend/index.html"));
	m_pWebViewWidget->loadLocalFile(m_strFrontendPath, QStringLiteral("dashboard"));
}

KMainWindow::~KMainWindow()
{
	m_pComposition->shutdown();
}

void KMainWindow::closeEvent(QCloseEvent *pEvent)
{
	if (!m_bShutdownComplete)
	{
		pEvent->ignore();
		if (!m_bClosePending)
		{
			m_bClosePending = true;
			closeRemoteDesktopWindow();
			m_pComposition->shutdown();
		}
		return;
	}
	closeRemoteDesktopWindow();
	QMainWindow::closeEvent(pEvent);
}

bool KMainWindow::nativeEvent(const QByteArray &eventType, void *pMessage, qintptr *pResult)
{
	if (handleNativeHitTest(pMessage, pResult))
		return true;
	return QMainWindow::nativeEvent(eventType, pMessage, pResult);
}

void KMainWindow::initConnections()
{
	m_pComposition->wireDashboard(m_pWebViewWidget);
	connect(m_pWebViewWidget, &KWebViewWidget::enterDesktopRequested,
		this, &KMainWindow::openRemoteDesktopWindow);
	connect(m_pWebViewWidget, &KWebViewWidget::minimizeMainWindowRequested,
		this, &KMainWindow::showMinimized);
	connect(m_pWebViewWidget, &KWebViewWidget::closeMainWindowRequested,
		this, &KMainWindow::close);
	connect(m_pWebViewWidget, &KWebViewWidget::beginMainWindowDragRequested,
		this, &KMainWindow::beginWindowDrag);
}

void KMainWindow::openRemoteDesktopWindow()
{
	if (m_pRemoteDesktopWindow == nullptr)
	{
		m_pRemoteDesktopWindow = new KRemoteDesktopWindow(this);
		connect(m_pRemoteDesktopWindow, &KRemoteDesktopWindow::desktopCloseRequested,
			this, &KMainWindow::closeRemoteDesktopWindow);
		m_pComposition->wireRemoteDesktopWindow(m_pRemoteDesktopWindow);
		m_pRemoteDesktopWindow->loadFrontend(m_strFrontendPath);
	}

	m_pRemoteDesktopWindow->show();
	m_pRemoteDesktopWindow->raise();
	m_pRemoteDesktopWindow->activateWindow();
	m_pComposition->enterRemoteDesktop();
}

void KMainWindow::closeRemoteDesktopWindow()
{
	if (m_pRemoteDesktopWindow == nullptr)
		return;

	m_pComposition->leaveRemoteDesktop();
	m_pRemoteDesktopWindow->deleteLater();
	m_pRemoteDesktopWindow = nullptr;
}

void KMainWindow::beginWindowDrag()
{
	::ReleaseCapture();
	::SendMessageW(reinterpret_cast<HWND>(winId()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void KMainWindow::applyWindowCorners()
{
	const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
	::DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()),
		DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

bool KMainWindow::handleNativeHitTest(void *pMessage, qintptr *pResult) const
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
