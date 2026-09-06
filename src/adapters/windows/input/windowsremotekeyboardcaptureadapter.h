#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_INPUT_WINDOWSREMOTEKEYBOARDCAPTUREADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_INPUT_WINDOWSREMOTEKEYBOARDCAPTUREADAPTER_H_

#include "core/input/keyboardshortcut.h"

#include <QtCore/QObject>

#include <Windows.h>

#include <functional>

struct KWindowsKeyboardEvent
{
	quint32 nVirtualKey = 0;
	quint32 nScanCode = 0;
	quint32 nFlags = 0;
	bool bPressed = false;
};

class KWindowsRemoteKeyboardCaptureAdapter final : public QObject
{
	Q_OBJECT

public:
	using EventHandler = std::function<bool(const KWindowsKeyboardEvent &event)>;

	explicit KWindowsRemoteKeyboardCaptureAdapter(QObject *pParent = nullptr);
	~KWindowsRemoteKeyboardCaptureAdapter() override;

	bool start(EventHandler handler, QString *pError);
	void stop();
	bool isActive() const;
	void replayLocalShortcut(const KKeyboardShortcut &shortcut);
	static quintptr replayMarker();

private:
	static LRESULT CALLBACK keyboardHookProcedure(int nCode, WPARAM wParam, LPARAM lParam);
	static KWindowsRemoteKeyboardCaptureAdapter *s_pActiveAdapter;

	HHOOK m_hKeyboardHook = nullptr;
	EventHandler m_eventHandler;
};

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_INPUT_WINDOWSREMOTEKEYBOARDCAPTUREADAPTER_H_
