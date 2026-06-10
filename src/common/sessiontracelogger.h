#ifndef _WINREMOTECONTROL_SESSIONTRACELOGGER_H_
#define _WINREMOTECONTROL_SESSIONTRACELOGGER_H_

#include <QtCore/QString>

class KSessionTraceLogger
{
public:
	static bool isEnabled();
	static void write(const QString &strRole,
		const QString &strStage,
		const QString &strType,
		int nSize = -1,
		const QString &strExtra = QString());

private:
	KSessionTraceLogger() = delete;
	~KSessionTraceLogger() = delete;
};

#endif // _WINREMOTECONTROL_SESSIONTRACELOGGER_H_
