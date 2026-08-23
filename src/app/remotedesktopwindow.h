#ifndef _WINREMOTECONTROL_REMOTEDESKTOPWINDOW_H_
#define _WINREMOTECONTROL_REMOTEDESKTOPWINDOW_H_

#include "core/media/streamconfig.h"
#include "core/privacy/privacytypes.h"
#include "core/protocol/sessionmessage.h"
#include "core/session/sessionstatemachine.h"

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
	void handleSessionStateChanged(KSessionState state);
	void handleSessionCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void handlePrivacyModeStatusChanged(const KPrivacyModeStatus &status);
	void handlePostSessionActionStatusChanged(const KPostSessionActionStatus &status);
	void handlePrivacyModeCommandStarted();
	void handlePostSessionActionCommandStarted();
	void handleSecurityPreferenceError(const QString &strError);
	void handlePrivacyModeCommandCompleted(const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode);
	void handlePostSessionActionCommandCompleted(const QString &strRequestId,
		bool bSuccess,
		const QString &strErrorCode);

signals:
	void desktopCloseRequested();
	void streamConfigRequested(const KStreamConfig &config);
	void privacyModeRequested(KPrivacyMode mode);
	void postSessionActionRequested(KPostSessionAction action);

protected:
	void closeEvent(QCloseEvent *pEvent) override;
	bool nativeEvent(const QByteArray &eventType, void *pMessage, qintptr *pResult) override;

private:
	void initConnections();
	void updatePreviewRect(const QRect &rect);
	void showControlCenterMenu(const QPoint &pos);
	void showSecurityCommandError(const QString &strErrorCode);
	void applyStreamConfig(int nWidth, int nHeight, int nFps, int nBitrateKbps);
	void minimizeWindow();
	void toggleMaximizeWindow();
	void beginWindowDrag();
	void adjustInitialWindowSize(int nFrameWidth, int nFrameHeight);
	bool handleNativeHitTest(void *pMessage, qintptr *pResult) const;

	bool m_bClosing = false;
	bool m_bInitialSizeAdjusted = false;
	bool m_bSessionAvailable = true;
	bool m_bPrivacyCommandPending = false;
	bool m_bPostSessionActionCommandPending = false;
	KSessionState m_sessionState = IdleSessionState;
	QRect m_previewRect;
	KStreamConfig m_streamConfig;
	KNegotiatedCapabilities m_capabilities;
	KPrivacyModeStatus m_privacyModeStatus;
	KPostSessionActionStatus m_postSessionActionStatus;
	KWebViewWidget *m_pWebViewWidget = nullptr;
	KVideoRenderWidget *m_pVideoRenderWidget = nullptr;
};

#endif // _WINREMOTECONTROL_REMOTEDESKTOPWINDOW_H_
