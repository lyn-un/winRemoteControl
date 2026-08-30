#ifndef _WINREMOTECONTROL_MAINWINDOW_H_
#define _WINREMOTECONTROL_MAINWINDOW_H_

#include "core/media/videoencoderpreference.h"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtWidgets/QMainWindow>

class KRemoteDesktopWindow;
class KFileTransferWindow;
class KApplicationComposition;
class KWebViewWidget;

class KMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit KMainWindow(KVideoEncoderPreference encoderPreference,
		QWidget *pParent = nullptr);
	~KMainWindow() override;

	KMainWindow(const KMainWindow &) = delete;
	KMainWindow &operator=(const KMainWindow &) = delete;

protected:
	void closeEvent(QCloseEvent *pEvent) override;
	bool nativeEvent(const QByteArray &eventType, void *pMessage, qintptr *pResult) override;

private:
	void initConnections();
	void openRemoteDesktopWindow();
	void closeRemoteDesktopWindow();
	void openFileTransferWindow();
	void closeFileTransferWindow();
	void beginWindowDrag();
	void applyWindowCorners();
	bool handleNativeHitTest(void *pMessage, qintptr *pResult) const;

	QString m_strFrontendPath;
	KApplicationComposition *m_pComposition = nullptr;
	KWebViewWidget *m_pWebViewWidget = nullptr;
	KRemoteDesktopWindow *m_pRemoteDesktopWindow = nullptr;
	KFileTransferWindow *m_pFileTransferWindow = nullptr;
	bool m_bClosePending = false;
	bool m_bShutdownComplete = false;
};

#endif // _WINREMOTECONTROL_MAINWINDOW_H_
