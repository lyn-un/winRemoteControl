#ifndef _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
#define _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_

#include <QtCore/QObject>

class KCaptureService;
class KSessionViewModel;
class KWebRtcSessionService;

class KApplicationComposition : public QObject
{
public:
	explicit KApplicationComposition(QObject *pParent = nullptr);
	~KApplicationComposition() override;

	KApplicationComposition(const KApplicationComposition &) = delete;
	KApplicationComposition &operator=(const KApplicationComposition &) = delete;

	KSessionViewModel *sessionViewModel() const;
	void shutdown();

private:
	void wireServices();

	KCaptureService *m_pCaptureService = nullptr;
	KWebRtcSessionService *m_pSessionService = nullptr;
	KSessionViewModel *m_pSessionViewModel = nullptr;
	bool m_bShutdown = false;
};

#endif // _WINREMOTECONTROL_APPLICATIONCOMPOSITION_H_
