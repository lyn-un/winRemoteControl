#ifndef _WINREMOTECONTROL_MAINWINDOW_H_
#define _WINREMOTECONTROL_MAINWINDOW_H_

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtWidgets/QMainWindow>

class KRemoteDesktopWindow;
class KApplicationComposition;
class KWebViewWidget;
class KRemoteTerminalWindow;

class KMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit KMainWindow(QWidget *pParent = nullptr);
	~KMainWindow() override;

	KMainWindow(const KMainWindow &) = delete;
	KMainWindow &operator=(const KMainWindow &) = delete;

protected:
	void closeEvent(QCloseEvent *pEvent) override;

private:
	void initConnections();
	void openRemoteDesktopWindow();
	void closeRemoteDesktopWindow();
	void openRemoteTerminalWindow();
	void closeRemoteTerminalWindow();

	QString m_strFrontendPath;
	KApplicationComposition *m_pComposition = nullptr;
	KWebViewWidget *m_pWebViewWidget = nullptr;
	KRemoteDesktopWindow *m_pRemoteDesktopWindow = nullptr;
	KRemoteTerminalWindow *m_pRemoteTerminalWindow = nullptr;
	bool m_bClosePending = false;
	bool m_bShutdownComplete = false;
};

#endif // _WINREMOTECONTROL_MAINWINDOW_H_
