#ifndef _WINREMOTECONTROL_LATENCYTRACELOGGER_H_
#define _WINREMOTECONTROL_LATENCYTRACELOGGER_H_

#include <QtCore/QString>

class KLatencyTraceLogger
{
public:
	static bool isEnabled();
	static void write(const QString &strSide,
		const QString &strStage,
		const QString &strExtra = QString());

private:
	KLatencyTraceLogger() = delete;
	~KLatencyTraceLogger() = delete;
};

#endif // _WINREMOTECONTROL_LATENCYTRACELOGGER_H_
