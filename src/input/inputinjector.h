#ifndef _WINREMOTECONTROL_INPUTINJECTOR_H_
#define _WINREMOTECONTROL_INPUTINJECTOR_H_

#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QString>

class KInputInjector : public QObject
{
	Q_OBJECT

public:
	explicit KInputInjector(QObject *pParent = nullptr);
	~KInputInjector() override;

	KInputInjector(const KInputInjector &) = delete;
	KInputInjector &operator=(const KInputInjector &) = delete;

public slots:
	void handleInputMessage(const QString &strMessage);
	void releaseAllKeys();
	void releaseAllInputs();

signals:
	void inputError(const QString &strMessage);
	void inputInjected(quint64 nSeq, qint64 nInjectedMs);

private:
	bool sendMouseMove(int nX, int nY, QString *pErrorMessage);
	bool sendMouseButton(int nX, int nY, const QString &strButton, bool bPressed, QString *pErrorMessage);
	bool sendMouseWheel(int nX, int nY, int nDelta, QString *pErrorMessage);
	bool sendKey(int nVirtualKey, bool bPressed, bool bExtended, QString *pErrorMessage);
	void releaseAllMouseButtons();
	static int clampToRange(int nValue, int nMinValue, int nMaxValue);
	static QString lastWin32ErrorMessage(const QString &strPrefix);

	QSet<quint32> m_pressedKeys;
	QSet<QString> m_pressedMouseButtons;
};

#endif // _WINREMOTECONTROL_INPUTINJECTOR_H_
