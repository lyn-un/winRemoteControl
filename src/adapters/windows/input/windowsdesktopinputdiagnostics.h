#ifndef _WINREMOTECONTROL_WINDOWSDESKTOPINPUTDIAGNOSTICS_H_
#define _WINREMOTECONTROL_WINDOWSDESKTOPINPUTDIAGNOSTICS_H_

#include <QtCore/QString>
#include <QtCore/QStringList>

enum KDesktopInputWindowKind
{
	OtherDesktopInputWindowKind,
	DesktopListViewInputWindowKind,
	ShellDefViewInputWindowKind,
	WorkerInputWindowKind,
	ProgramManagerInputWindowKind,
	N0vaDesktopInputWindowKind
};

class KWindowsDesktopInputDiagnostics
{
public:
	KWindowsDesktopInputDiagnostics();
	~KWindowsDesktopInputDiagnostics();

	KWindowsDesktopInputDiagnostics(const KWindowsDesktopInputDiagnostics &) = delete;
	KWindowsDesktopInputDiagnostics &operator=(
		const KWindowsDesktopInputDiagnostics &) = delete;

	quint64 beginMouseButton(int nX,
		int nY,
		int nButton,
		bool bPressed,
		bool bDuplicateState);
	void completeMouseButton(quint64 nEventId,
		unsigned int nInsertedInputs,
		unsigned long nWin32Error);
	quint64 beginMouseMove(int nX, int nY, bool bDragging);
	void completeMouseMove(quint64 nEventId,
		unsigned int nInsertedInputs,
		unsigned long nWin32Error);
	void reset();

	static KDesktopInputWindowKind classifyWindow(
		const QString &strClassName, const QString &strProcessFileName);
	static bool isDesktopListViewHierarchy(const QString &strProcessFileName,
		const QStringList &ancestorClassNames);
	static QString processFileNameToken(const QString &strProcessPath);
	static QString integrityLevelName(unsigned long nIntegrityRid);
	static QString mouseHookFlagsToken(unsigned long nFlags);

private:
	void ensureObserversStarted();
	void writePendingDragLast();

	void *m_pWinEventHook = nullptr;
	void *m_pMouseHook = nullptr;
	quint64 m_nNextEventId = 1;
	quint64 m_nButtonStateInterval = 0;
	int m_nLastButtonX = 0;
	int m_nLastButtonY = 0;
	int m_nLastDragX = 0;
	int m_nLastDragY = 0;
	unsigned int m_nLastDragInsertedInputs = 0;
	unsigned long m_nLastDragWin32Error = 0;
	bool m_bHasPendingDrag = false;
	bool m_bFirstDragMoveLogged = false;
	bool m_bEnabled = false;
	bool m_bObserversStarted = false;
};

#endif // _WINREMOTECONTROL_WINDOWSDESKTOPINPUTDIAGNOSTICS_H_
