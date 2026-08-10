#ifndef _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
#define _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_

#include <QtCore/QObject>

class KCaptureService;
class KRemoteDesktopWindow;
class KSessionViewModel;
class KWebViewWidget;
class KSessionCoordinator;
class KDeviceDiscoveryController;
class KDeviceDiscoveryViewModel;
class KRecentDeviceService;
class KApplicationSettingsService;
class KClipboardSyncService;
class KTerminalSessionService;
class QTimer;

class KApplicationComposition : public QObject
{
	Q_OBJECT

public:
	explicit KApplicationComposition(QObject *pParent = nullptr);
	~KApplicationComposition() override;

	KApplicationComposition(const KApplicationComposition &) = delete;
	KApplicationComposition &operator=(const KApplicationComposition &) = delete;

	KSessionViewModel *sessionViewModel() const;
	void wireDashboard(KWebViewWidget *pWebViewWidget);
	void wireRemoteDesktopWindow(KRemoteDesktopWindow *pWindow);
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
	KSessionCoordinator *m_pSessionService = nullptr;
	KSessionViewModel *m_pSessionViewModel = nullptr;
	KDeviceDiscoveryController *m_pDiscoveryService = nullptr;
	KDeviceDiscoveryViewModel *m_pDiscoveryViewModel = nullptr;
	KRecentDeviceService *m_pRecentDeviceService = nullptr;
	KApplicationSettingsService *m_pApplicationSettingsService = nullptr;
	KClipboardSyncService *m_pClipboardSyncService = nullptr;
	KTerminalSessionService *m_pTerminalSessionService = nullptr;
	QTimer *m_pShutdownDeadlineTimer = nullptr;
	bool m_bShutdown = false;
	bool m_bShutdownFinished = false;
	bool m_bSessionShutdownPending = false;
	bool m_bCaptureShutdownPending = false;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
