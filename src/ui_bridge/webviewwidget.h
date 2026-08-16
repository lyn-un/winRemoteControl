#ifndef _WINREMOTECONTROL_WEBVIEWWIDGET_H_
#define _WINREMOTECONTROL_WEBVIEWWIDGET_H_

#include "core/media/networkstats.h"
#include "core/media/streamconfig.h"
#include "core/discovery/discovereddevice.h"
#include "core/devices/recentdevice.h"
#include "core/settings/applicationsettings.h"
#include "core/session/sessionerror.h"
#include "core/protocol/sessionmessage.h"
#include "core/terminal/terminalstate.h"
#include "core/security/permissionscope.h"
#include "core/security/trusteddevice.h"

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
	void sendSessionError(const KSessionError &error);
	void sendFrameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
	void sendNetworkStatsChanged(const KNetworkStats &stats);
	void sendLanDevicesChanged(const QVector<KDiscoveredDevice> &devices);
	void sendLanDiscoveryError(const QString &strError);
	void sendRecentDevicesChanged(const QVector<KRecentDevice> &devices);
	void sendRecentDeviceError(const QString &strError);
	void sendApplicationSettingsChanged(const KApplicationSettings &settings);
	void sendApplicationSettingsError(const QString &strError);
	void sendIncomingAccessRequest(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strSourceAddress,
		qint64 nExpiresAtMs);
	void sendIncomingAccessRequestCleared(const QString &strRequestId, const QString &strReason);
	void sendPairingRequest(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strLocalRole,
		const QString &strVerificationCode,
		const QString &strControllerFingerprint,
		const QString &strControlledFingerprint,
		const QString &strTlsProtocol,
		const QString &strCipherSuite,
		KPermissionScopes requestedPermissions,
		qint64 nExpiresAtMs);
	void sendPairingCleared(const QString &strRequestId, const QString &strReason);
	void sendDeviceAuthenticationStateChanged(const QString &strState,
		const QString &strDeviceId,
		const QString &strFingerprint,
		bool bTrusted);
	void sendTrustedDevicesChanged(const QVector<KTrustedDevice> &devices);
	void sendTrustedDeviceError(const QString &strError);
	void sendSecurityMigrationNotice(const QString &strMessage);
	void sendSessionPermissionsChanged(KPermissionScopes permissions);
	void sendClipboardSyncStateChanged(bool bEnabled,
		bool bAvailable,
		bool bActive,
		const QString &strStatus);
	void sendClipboardSyncError(const QString &strError);
	void sendSessionCapabilitiesChanged(const KNegotiatedCapabilities &capabilities);
	void sendTerminalStateChanged(KTerminalState state,
		bool bAvailable,
		const QString &strStatus,
		const QString &strDeviceName,
		const QString &strDeviceSource);
	void sendTerminalFrontendSupportChanged(bool bSupported, const QString &strReason);
	void sendIncomingTerminalRequest(const QString &strRequestId,
		const QString &strDeviceName,
		const QString &strDeviceSource,
		qint64 nExpiresAtMs);
	void sendIncomingTerminalRequestCleared(const QString &strRequestId,
		const QString &strReason);
	void sendTerminalError(const QString &strError);

signals:
	void startCaptureRequested();
	void stopCaptureRequested();
	void setRoleRequested(const QString &strRole);
	void startSignalingServerRequested(quint16 nPort);
	void connectSignalingRequested(const QString &strHost, quint16 nPort);
	void retryLastConnectionRequested();
	void setClipboardSyncEnabledRequested(bool bEnabled);
	void requestClipboardSyncStateRequested();
	void refreshLanDevicesRequested();
	void connectLanDeviceRequested(const QString &strDeviceId);
	void requestRecentDevicesRequested();
	void connectRecentDeviceRequested(const QString &strDeviceId);
	void removeRecentDeviceRequested(const QString &strDeviceId);
	void openRecentDeviceTerminalRequested(const QString &strDeviceId);
	void openCurrentTerminalRequested();
	void requestTerminalFrontendSupportRequested();
	void respondTerminalAccessRequestRequested(const QString &strRequestId, bool bAccepted);
	void closeTerminalRequested();
	void requestApplicationSettingsRequested();
	void updateApplicationSettingsRequested(bool bRemoteAccessEnabled,
		const QString &strApprovalMode,
		int nApprovalTimeoutSeconds,
		int nDefaultListenPort);
	void respondIncomingAccessRequestRequested(const QString &strRequestId, bool bAccepted);
	void respondPairingRequestRequested(const QString &strRequestId,
		bool bAccepted,
		KPermissionScopes permissions);
	void requestTrustedDevicesRequested();
	void updateTrustedDeviceRequested(const QString &strDeviceId,
		const QString &strAlias,
		KPermissionScopes permissions);
	void revokeTrustedDeviceRequested(const QString &strDeviceId);
	void requestRePairDeviceRequested(const QString &strDeviceId);
	void disconnectSessionRequested();
	void startStreamingRequested();
	void stopStreamingRequested();
	void enterDesktopRequested();
	void minimizeMainWindowRequested();
	void closeMainWindowRequested();
	void beginMainWindowDragRequested();
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
