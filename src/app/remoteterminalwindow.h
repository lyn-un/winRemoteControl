#ifndef _WINREMOTECONTROL_REMOTETERMINALWINDOW_H_
#define _WINREMOTECONTROL_REMOTETERMINALWINDOW_H_

#include <QtWidgets/QMainWindow>

class KWebViewWidget;

class KRemoteTerminalWindow final : public QMainWindow
{
	Q_OBJECT

public:
	explicit KRemoteTerminalWindow(QWidget *pParent = nullptr);
	~KRemoteTerminalWindow() override;

	KRemoteTerminalWindow(const KRemoteTerminalWindow &) = delete;
	KRemoteTerminalWindow &operator=(const KRemoteTerminalWindow &) = delete;

	KWebViewWidget *webViewWidget() const;
	void loadFrontend(const QString &strFrontendPath);

signals:
	void terminalCloseRequested();

protected:
	void closeEvent(QCloseEvent *pEvent) override;

private:
	KWebViewWidget *m_pWebViewWidget = nullptr;
};

#endif // _WINREMOTECONTROL_REMOTETERMINALWINDOW_H_
