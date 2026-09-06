#ifndef _WINREMOTECONTROL_DESKTOPINPUTTRACELOGGER_H_
#define _WINREMOTECONTROL_DESKTOPINPUTTRACELOGGER_H_

#include <QtCore/QString>

class KDesktopInputTraceLogger
{
public:
	static void configure(bool bEnabled, const QString &strLogDirectory);
	static bool isEnabled();
	static void write(const QString &strStage, const QString &strExtra = QString());
	static void shutdown();

private:
	KDesktopInputTraceLogger() = delete;
	~KDesktopInputTraceLogger() = delete;
};

#endif // _WINREMOTECONTROL_DESKTOPINPUTTRACELOGGER_H_
