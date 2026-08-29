#ifndef _WINREMOTECONTROL_FILETRANSFERWINDOW_H_
#define _WINREMOTECONTROL_FILETRANSFERWINDOW_H_

#include <QtWidgets/QMainWindow>

class KWebViewWidget;

class KFileTransferWindow final : public QMainWindow
{
	Q_OBJECT

public:
	explicit KFileTransferWindow(QWidget *pParent = nullptr);
	~KFileTransferWindow() override;

	KFileTransferWindow(const KFileTransferWindow &) = delete;
	KFileTransferWindow &operator=(const KFileTransferWindow &) = delete;

	KWebViewWidget *webViewWidget() const;
	void loadFrontend(const QString &strFrontendPath, const QString &strThemeId);
	void setHasActiveTasks(bool bHasActiveTasks);

signals:
	void fileTransferCloseRequested(bool bStopLogicalSession);

protected:
	void closeEvent(QCloseEvent *pEvent) override;
	bool nativeEvent(const QByteArray &eventType, void *pMessage, qintptr *pResult) override;

private:
	void initConnections();
	void toggleMaximizeWindow();
	void beginWindowDrag();
	void applyWindowCorners();
	bool handleNativeHitTest(void *pMessage, qintptr *pResult) const;

	KWebViewWidget *m_pWebViewWidget = nullptr;
	bool m_bClosing = false;
	bool m_bHasActiveTasks = false;
	bool m_bCloseConfirmed = false;
};

#endif // _WINREMOTECONTROL_FILETRANSFERWINDOW_H_
