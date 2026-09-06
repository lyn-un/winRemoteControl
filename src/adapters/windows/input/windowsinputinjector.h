#ifndef _WINREMOTECONTROL_WINDOWSINPUTINJECTOR_H_
#define _WINREMOTECONTROL_WINDOWSINPUTINJECTOR_H_

#include "core/input/inputinjectorinterface.h"

#include <QtCore/QSet>

#include <memory>

class KWindowsDesktopInputDiagnostics;

class KWindowsInputInjector final : public IKInputInjector
{
public:
	KWindowsInputInjector();
	~KWindowsInputInjector() override;

	bool inject(const KInputMessage &message, QString *pErrorMessage) override;
	void releaseAllKeys(QStringList *pErrorMessages) override;
	void releaseAllInputs(QStringList *pErrorMessages) override;

private:
	bool sendMouseMove(int nX, int nY, QString *pErrorMessage);
	bool sendMouseButton(int nX,
		int nY,
		KRemoteMouseButton button,
		bool bPressed,
		QString *pErrorMessage);
	bool sendMouseWheel(int nX, int nY, int nDelta, QString *pErrorMessage);
	bool sendKey(int nVirtualKey,
		int nScanCode,
		bool bPressed,
		bool bExtended,
		bool bAutoRepeat,
		QString *pErrorMessage);
	bool sendText(const QString &strText, QString *pErrorMessage);
	void releaseAllMouseButtons(QStringList *pErrorMessages);
	static QString lastWin32ErrorMessage(const QString &strPrefix);

	QSet<quint32> m_pressedKeys;
	QSet<int> m_pressedMouseButtons;
	std::unique_ptr<KWindowsDesktopInputDiagnostics> m_spDesktopInputDiagnostics;
};

#endif // _WINREMOTECONTROL_WINDOWSINPUTINJECTOR_H_
