#include "app/filetransferwindow.h"

#include "ui_bridge/webviewwidget.h"

#include <QtGui/QCloseEvent>

#include <Windows.h>
#include <windowsx.h>

namespace
{
	constexpr int kResizeBorderDip = 8;
}

KFileTransferWindow::KFileTransferWindow(QWidget *pParent)
	: QMainWindow(pParent)
	, m_pWebViewWidget(new KWebViewWidget(this))
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
	setWindowTitle(QStringLiteral("winRemoteControl File Transfer"));
	setMinimumSize(960, 640);
	resize(1380, 820);
	setCentralWidget(m_pWebViewWidget);
	initConnections();
}

KFileTransferWindow::~KFileTransferWindow()
{
}

KWebViewWidget *KFileTransferWindow::webViewWidget() const
{
	return m_pWebViewWidget;
}

void KFileTransferWindow::loadFrontend(
	const QString &strFrontendPath,
	const QString &strThemeId)
{
	m_pWebViewWidget->loadLocalFile(
		strFrontendPath, QStringLiteral("fileTransfer"), strThemeId);
}

void KFileTransferWindow::setHasActiveTasks(bool bHasActiveTasks)
{
	m_bHasActiveTasks = bHasActiveTasks;
}

void KFileTransferWindow::closeEvent(QCloseEvent *pEvent)
{
	if (m_bHasActiveTasks && !m_bCloseConfirmed)
	{
		pEvent->ignore();
		m_pWebViewWidget->sendFileTransferClosePromptRequested();
		return;
	}
	if (!m_bClosing)
	{
		m_bClosing = true;
		emit fileTransferCloseRequested(!m_bHasActiveTasks);
	}
	QMainWindow::closeEvent(pEvent);
}

bool KFileTransferWindow::nativeEvent(
	const QByteArray &eventType,
	void *pMessage,
	qintptr *pResult)
{
	if (eventType == QByteArrayLiteral("windows_generic_MSG")
		&& handleNativeHitTest(pMessage, pResult))
	{
		return true;
	}
	return QMainWindow::nativeEvent(eventType, pMessage, pResult);
}

void KFileTransferWindow::initConnections()
{
	connect(m_pWebViewWidget, &KWebViewWidget::closeFileTransferWindowRequested,
		this, [this]()
		{
			m_bCloseConfirmed = true;
			close();
		});
	connect(m_pWebViewWidget, &KWebViewWidget::minimizeFileTransferWindowRequested,
		this, &KFileTransferWindow::showMinimized);
	connect(m_pWebViewWidget, &KWebViewWidget::toggleMaximizeFileTransferWindowRequested,
		this, &KFileTransferWindow::toggleMaximizeWindow);
	connect(m_pWebViewWidget, &KWebViewWidget::beginFileTransferWindowDragRequested,
		this, &KFileTransferWindow::beginWindowDrag);
}

void KFileTransferWindow::toggleMaximizeWindow()
{
	if (isMaximized())
		showNormal();
	else
		showMaximized();
}

void KFileTransferWindow::beginWindowDrag()
{
	::ReleaseCapture();
	::SendMessageW(reinterpret_cast<HWND>(winId()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

bool KFileTransferWindow::handleNativeHitTest(void *pMessage, qintptr *pResult) const
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
