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

class KApplicationComposition : public QObject
{
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

private:
	void wireServices();

	KCaptureService *m_pCaptureService = nullptr;
	KSessionCoordinator *m_pSessionService = nullptr;
	KSessionViewModel *m_pSessionViewModel = nullptr;
	KDeviceDiscoveryController *m_pDiscoveryService = nullptr;
	KDeviceDiscoveryViewModel *m_pDiscoveryViewModel = nullptr;
	bool m_bShutdown = false;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
