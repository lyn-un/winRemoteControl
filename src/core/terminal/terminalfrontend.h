#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALFRONTEND_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALFRONTEND_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

class KTerminalFrontend : public QObject
{
	Q_OBJECT

public:
	explicit KTerminalFrontend(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}
	~KTerminalFrontend() override = default;

	virtual bool isSupported(QString *pReason = nullptr) const = 0;
	virtual bool open(quint64 nGeneration, const QString &strTitle,
		QString *pErrorMessage = nullptr) = 0;
	virtual void focus() = 0;
	virtual bool writeOutput(quint64 nGeneration, const QByteArray &data) = 0;
	virtual void setInputPaused(bool bPaused) = 0;
	virtual void close(quint64 nGeneration) = 0;

signals:
	void connected(quint64 nGeneration);
	void inputReady(quint64 nGeneration, const QByteArray &data);
	void resizeRequested(quint64 nGeneration, int nColumns, int nRows);
	void closed(quint64 nGeneration);
	void terminalError(quint64 nGeneration, const QString &strErrorCode,
		const QString &strTechnicalMessage);
};

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALFRONTEND_H_
