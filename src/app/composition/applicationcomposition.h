#ifndef _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
#define _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_

#include "core/media/videoencoderpreference.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

class KCaptureService;
class KApplicationCommandRegistry;
class KAutomationHostBridge;
class KAutomationPluginLoader;
class KRemoteDesktopWindow;
class KFileTransferWindow;
class KSessionViewModel;
class KWebViewWidget;
class KSessionCoordinator;
class KDeviceDiscoveryController;
class KDeviceDiscoveryViewModel;
class KRecentDeviceService;
class KApplicationSettingsService;
class KDeviceSecurityPreferenceService;
class KClipboardSyncService;
class KTerminalSessionService;
class KFileTransferSessionService;
class QTimer;

class KApplicationComposition : public QObject
{
	Q_OBJECT

public:
	explicit KApplicationComposition(KVideoEncoderPreference encoderPreference,
		QObject *pParent = nullptr);
	~KApplicationComposition() override;

	KApplicationComposition(const KApplicationComposition &) = delete;
	KApplicationComposition &operator=(const KApplicationComposition &) = delete;

	KSessionViewModel *sessionViewModel() const;
	KApplicationCommandRegistry *applicationCommandRegistry() const;
	QString applicationThemeId() const;
	void wireDashboard(KWebViewWidget *pWebViewWidget);
	void wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow);
	void wireFileTransferWindow(KFileTransferWindow *pWindow);
	void enterRemoteDesktop();
	void leaveRemoteDesktop();
	void disconnectSession();
	void shutdown();

signals:
	void shutdownFinished();

private:
	void wireServices();
	void tryFinishShutdown();
	void handleShutdownDeadline();

	KCaptureService *m_pCaptureService = nullptr;
	KApplicationCommandRegistry *m_pApplicationCommandRegistry = nullptr;
	KAutomationHostBridge *m_pAutomationHostBridge = nullptr;
	std::unique_ptr<KAutomationPluginLoader> m_spAutomationPluginLoader;
	KSessionCoordinator *m_pSessionService = nullptr;
	KSessionViewModel *m_pSessionViewModel = nullptr;
	KDeviceDiscoveryController *m_pDiscoveryService = nullptr;
	KDeviceDiscoveryViewModel *m_pDiscoveryViewModel = nullptr;
	KRecentDeviceService *m_pRecentDeviceService = nullptr;
	KApplicationSettingsService *m_pApplicationSettingsService = nullptr;
	KDeviceSecurityPreferenceService *m_pDeviceSecurityPreferenceService = nullptr;
	KClipboardSyncService *m_pClipboardSyncService = nullptr;
	KTerminalSessionService *m_pTerminalSessionService = nullptr;
	KFileTransferSessionService *m_pFileTransferSessionService = nullptr;
	QTimer *m_pShutdownDeadlineTimer = nullptr;
	QString m_strFileTransferStatus = QStringLiteral("closed");
	QString m_strFileTransferDeviceName;
	int m_nFileTransferActiveTaskCount = 0;
	bool m_bShutdown = false;
	bool m_bShutdownFinished = false;
	bool m_bSessionShutdownPending = false;
	bool m_bCaptureShutdownPending = false;
	bool m_bFileTransferShutdownPending = false;
	int m_nLastResourceTraceState = 0;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
