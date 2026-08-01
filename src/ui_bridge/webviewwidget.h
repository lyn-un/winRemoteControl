#ifndef _WINREMOTECONTROL_WEBVIEWWIDGET_H_
#define _WINREMOTECONTROL_WEBVIEWWIDGET_H_

#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/discovery/discovereddevice.h"
#include "core/devices/recentdevice.h"

#include <QtCore/QByteArray>
#include <QtCore/QPoint>
#include <QtCore/QQueue>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <Windows.h>
#include <unknwn.h>
#include <WebView2.h>
#include <wrl/client.h>

#include <string>

class KWebViewWidget : public QWidget
{
	Q_OBJECT

public:
	explicit KWebViewWidget(QWidget *pParent = nullptr);
	~KWebViewWidget() override;

	KWebViewWidget(const KWebViewWidget &) = delete;
	KWebViewWidget &operator=(const KWebViewWidget &) = delete;

	void loadLocalFile(const QString &strFilePath, const QString &strViewMode = QStringLiteral("dashboard"));

public slots:
	void sendStatusChanged(const QString &strStatus);
	void sendSignalingChanged(const QString &strState);
	void sendWebRtcStateChanged(const QString &strState);
	void sendSessionChannelChanged(bool bOpen);
	void sendDeviceInfoChanged(const QString &strComputerName,
		const QString &strWallpaperMime,
		const QString &strWallpaperData,
		int nScreenWidth,
		int nScreenHeight);
	void sendCaptureError(const QString &strMessage);
	void sendFrameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void sendNetworkStatsChanged(const KNetworkStats &stats);
	void sendLanDevicesChanged(const QVector<KDiscoveredDevice> &devices);
	void sendLanDiscoveryError(const QString &strError);
	void sendRecentDevicesChanged(const QVector<KRecentDevice> &devices);
	void sendRecentDeviceError(const QString &strError);

signals:
	void startCaptureRequested();
	void stopCaptureRequested();
	void setRoleRequested(const QString &strRole);
	void startSignalingServerRequested(quint16 nPort);
	void connectSignalingRequested(const QString &strHost, quint16 nPort);
	void refreshLanDevicesRequested();
	void connectLanDeviceRequested(const QString &strDeviceId);
	void requestRecentDevicesRequested();
	void connectRecentDeviceRequested(const QString &strDeviceId);
	void removeRecentDeviceRequested(const QString &strDeviceId);
	void disconnectSessionRequested();
	void startStreamingRequested();
	void stopStreamingRequested();
	void enterDesktopRequested();
	void closeDesktopRequested();
	void minimizeDesktopWindowRequested();
	void toggleMaximizeDesktopWindowRequested();
	void beginDesktopWindowDragRequested();
	void showControlCenterMenuRequested(const QPoint &pos);
	void streamConfigRequested(const KStreamConfig &config);
	void previewRectChanged(const QRect &rect);

protected:
	void resizeEvent(QResizeEvent *pEvent) override;
	void showEvent(QShowEvent *pEvent) override;

private:
	void initializeWebView();
	void resizeWebView();
	void navigateIfReady();
	void handleWebMessage(const QString &strMessage);
	void postJson(const QString &strJson);
	void flushPendingMessages();
	static std::wstring toWideString(const QString &strValue);

	QString m_strPendingUrl;
	QString m_strFrontendFolder;
	bool m_bInitializing = false;
	bool m_bNavigationReady = false;
	QQueue<QString> m_pendingMessages;
	Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_spEnvironment;
	Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_spController;
	Microsoft::WRL::ComPtr<ICoreWebView2> m_spWebView;
};

#endif // _WINREMOTECONTROL_WEBVIEWWIDGET_H_
