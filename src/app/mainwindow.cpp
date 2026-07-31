#include "app/mainwindow.h"

#include "app/composition/applicationcomposition.h"
#include "app/remotedesktopwindow.h"
#include "ui_bridge/webviewwidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtGui/QCloseEvent>

KMainWindow::KMainWindow(QWidget *pParent)
	: QMainWindow(pParent)
	, m_pComposition(new KApplicationComposition(this))
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
	m_pComposition->shutdown();
}

void KMainWindow::closeEvent(QCloseEvent *pEvent)
{
	closeRemoteDesktopWindow();
	m_pComposition->disconnectSession();
	QMainWindow::closeEvent(pEvent);
}

void KMainWindow::initConnections()
{
	m_pComposition->wireDashboard(m_pWebViewWidget);
	connect(m_pWebViewWidget, &KWebViewWidget::enterDesktopRequested,
		this, &KMainWindow::openRemoteDesktopWindow);
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
