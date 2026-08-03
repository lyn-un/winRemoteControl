#ifndef _WINREMOTECONTROL_WINDOWSINPUTINJECTOR_H_
#define _WINREMOTECONTROL_WINDOWSINPUTINJECTOR_H_

#include "core/input/inputinjectorinterface.h"

#include <QtCore/QSet>

class KWindowsInputInjector final : public IKInputInjector
{
public:
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
	static int clampToRange(int nValue, int nMinValue, int nMaxValue);
	static QString lastWin32ErrorMessage(const QString &strPrefix);

	QSet<quint32> m_pressedKeys;
	QSet<int> m_pressedMouseButtons;
};

#endif // _WINREMOTECONTROL_WINDOWSINPUTINJECTOR_H_
