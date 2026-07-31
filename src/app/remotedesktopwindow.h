#ifndef _WINREMOTECONTROL_REMOTEDESKTOPWINDOW_H_
#define _WINREMOTECONTROL_REMOTEDESKTOPWINDOW_H_

#include "core/media/streamconfig.h"

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtWidgets/QMainWindow>

class KVideoRenderWidget;
class KWebViewWidget;

class KRemoteDesktopWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit KRemoteDesktopWindow(QWidget *pParent = nullptr);
	~KRemoteDesktopWindow() override;

	KRemoteDesktopWindow(const KRemoteDesktopWindow &) = delete;
	KRemoteDesktopWindow &operator=(const KRemoteDesktopWindow &) = delete;

	KWebViewWidget *webViewWidget() const;
	KVideoRenderWidget *videoRenderWidget() const;
	void loadFrontend(const QString &strFrontendPath);

public slots:
	void setRemoteScreenSize(int nWidth, int nHeight);
	void handleFrameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void handleSessionStateChanged(const QString &strState);

signals:
	void desktopCloseRequested();
	void streamConfigRequested(const KStreamConfig &config);

protected:
	void closeEvent(QCloseEvent *pEvent) override;
	bool nativeEvent(const QByteArray &eventType, void *pMessage, qintptr *pResult) override;

private:
	void initConnections();
	void updatePreviewRect(const QRect &rect);
	void showControlCenterMenu(const QPoint &pos);
	void applyStreamConfig(int nWidth, int nHeight, int nFps, int nBitrateKbps);
	void minimizeWindow();
	void toggleMaximizeWindow();
	void beginWindowDrag();
	void adjustInitialWindowSize(int nFrameWidth, int nFrameHeight);
	bool handleNativeHitTest(void *pMessage, qintptr *pResult) const;

	bool m_bClosing = false;
	bool m_bInitialSizeAdjusted = false;
	bool m_bSessionAvailable = true;
	QRect m_previewRect;
	KStreamConfig m_streamConfig;
	KWebViewWidget *m_pWebViewWidget = nullptr;
	KVideoRenderWidget *m_pVideoRenderWidget = nullptr;
};

#endif // _WINREMOTECONTROL_REMOTEDESKTOPWINDOW_H_
