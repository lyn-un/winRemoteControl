#ifndef _WINREMOTECONTROL_INPUTINJECTOR_H_
#define _WINREMOTECONTROL_INPUTINJECTOR_H_

#include <QtCore/QObject>
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

signals:
	void inputError(const QString &strMessage);

private:
	bool sendMouseMove(int nX, int nY, QString *pErrorMessage);
	bool sendMouseButton(int nX, int nY, const QString &strButton, bool bPressed, QString *pErrorMessage);
	bool sendMouseWheel(int nX, int nY, int nDelta, QString *pErrorMessage);
	static int clampToRange(int nValue, int nMinValue, int nMaxValue);
	static QString lastWin32ErrorMessage(const QString &strPrefix);
};

#endif // _WINREMOTECONTROL_INPUTINJECTOR_H_
