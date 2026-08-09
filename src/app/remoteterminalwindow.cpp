#include "app/remoteterminalwindow.h"

#include "ui_bridge/webviewwidget.h"

#include <QtGui/QCloseEvent>

KRemoteTerminalWindow::KRemoteTerminalWindow(QWidget *pParent)
	: QMainWindow(pParent)
	, m_pWebViewWidget(new KWebViewWidget(this))
{
	setWindowTitle(QStringLiteral("winRemoteControl Terminal"));
	resize(960, 620);
	setMinimumSize(640, 400);
	setCentralWidget(m_pWebViewWidget);
}

KRemoteTerminalWindow::~KRemoteTerminalWindow()
{
}

KWebViewWidget *KRemoteTerminalWindow::webViewWidget() const
{
	return m_pWebViewWidget;
}

void KRemoteTerminalWindow::loadFrontend(const QString &strFrontendPath)
{
	m_pWebViewWidget->loadLocalFile(strFrontendPath, QStringLiteral("terminal"));
}

void KRemoteTerminalWindow::closeEvent(QCloseEvent *pEvent)
{
	emit terminalCloseRequested();
	QMainWindow::closeEvent(pEvent);
}
