#ifndef _WINREMOTECONTROL_MAINWINDOW_H_
#define _WINREMOTECONTROL_MAINWINDOW_H_

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtWidgets/QMainWindow>

class KRemoteDesktopWindow;
class KSessionViewModel;
class KWebViewWidget;

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
	void wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow);

	QString m_strFrontendPath;
	KSessionViewModel *m_pSessionViewModel = nullptr;
	KWebViewWidget *m_pWebViewWidget = nullptr;
	KRemoteDesktopWindow *m_pRemoteDesktopWindow = nullptr;
};

#endif // _WINREMOTECONTROL_MAINWINDOW_H_
