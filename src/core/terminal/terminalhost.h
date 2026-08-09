#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALHOST_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALHOST_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

class KTerminalHost : public QObject
{
	Q_OBJECT

public:
	explicit KTerminalHost(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KTerminalHost() override = default;

	virtual bool isSupported(QString *pReason) const = 0;
	virtual bool start(quint64 nGeneration, int nColumns, int nRows,
		QString *pErrorMessage) = 0;
	virtual bool writeInput(quint64 nGeneration, const QByteArray &data) = 0;
	virtual bool resize(quint64 nGeneration, int nColumns, int nRows) = 0;
	virtual void requestStop(quint64 nGeneration) = 0;

signals:
	void outputReady(quint64 nGeneration, const QByteArray &data);
	void processExited(quint64 nGeneration, int nExitCode);
	void terminalError(quint64 nGeneration, const QString &strErrorCode,
		const QString &strTechnicalMessage);
	void stopped(quint64 nGeneration);
};

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALHOST_H_
