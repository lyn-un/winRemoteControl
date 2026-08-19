#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSPRIVACYOVERLAYADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSPRIVACYOVERLAYADAPTER_H_

#include "core/privacy/privacyoverlayadapter.h"

#include <QtCore/QObject>
#include <QtCore/QVector>

#include <Windows.h>

class QWidget;

class KWindowsPrivacyOverlayAdapter : public QObject, public IKPrivacyOverlayAdapter
{
	Q_OBJECT

public:
	explicit KWindowsPrivacyOverlayAdapter(QObject *pParent = nullptr);
	~KWindowsPrivacyOverlayAdapter() override;

	KWindowsPrivacyOverlayAdapter(const KWindowsPrivacyOverlayAdapter &) = delete;
	KWindowsPrivacyOverlayAdapter &operator=(const KWindowsPrivacyOverlayAdapter &) = delete;

	bool isSupported() const override;
	KPrivacyOperationResult apply() override;
	KPrivacyOperationResult restore() override;
	void setEmergencyRestoreHandler(EmergencyRestoreHandler handler) override;

	static bool isPhysicalRestoreShortcut(quint32 nVirtualKey,
		quint32 nFlags,
		bool bControlPressed,
		bool bAltPressed,
		bool bShiftPressed);

private slots:
	void handleScreenTopologyChanged();

private:
	static LRESULT CALLBACK keyboardHookProcedure(int nCode,
		WPARAM wParam,
		LPARAM lParam);
	bool installKeyboardHook();
	void removeKeyboardHook();
	void destroyWindows();
	void invokeEmergencyRestore();

	static KWindowsPrivacyOverlayAdapter *s_pActiveHookAdapter;
	QVector<QWidget *> m_windows;
	EmergencyRestoreHandler m_emergencyRestoreHandler;
	HHOOK m_hKeyboardHook = nullptr;
	bool m_bActive = false;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_PRIVACY_WINDOWSPRIVACYOVERLAYADAPTER_H_
