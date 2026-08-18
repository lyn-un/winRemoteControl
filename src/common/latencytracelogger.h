#ifndef _WINREMOTECONTROL_LATENCYTRACELOGGER_H_
#define _WINREMOTECONTROL_LATENCYTRACELOGGER_H_

#include <QtCore/QString>

class KLatencyTraceLogger
{
public:
	static void configure(bool bEnabled,
		const QString &strLogDirectory,
		const QString &strScenario);
	static bool isEnabled();
	static void write(const QString &strSide,
		const QString &strStage,
		const QString &strExtra = QString());
	static void shutdown();

private:
	KLatencyTraceLogger() = delete;
	~KLatencyTraceLogger() = delete;
};

#endif // _WINREMOTECONTROL_LATENCYTRACELOGGER_H_
