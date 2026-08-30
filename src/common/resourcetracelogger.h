#ifndef _WINREMOTECONTROL_COMMON_RESOURCETRACELOGGER_H_
#define _WINREMOTECONTROL_COMMON_RESOURCETRACELOGGER_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

struct KProcessResourceSnapshot
{
	quint64 nPrivateBytes = 0;
	quint64 nWorkingSetBytes = 0;
	quint64 nGpuDedicatedBytes = 0;
	quint64 nGpuSharedBytes = 0;
	quint32 nHandleCount = 0;
	quint32 nThreadCount = 0;
	bool bMemoryAvailable = false;
	bool bGpuAvailable = false;
	bool bHandleCountAvailable = false;
	bool bThreadCountAvailable = false;
};

class KResourceTraceLogger
{
public:
	static void configure(bool bEnabled, const QString &strLogDirectory);
	static bool isEnabled();
	static KProcessResourceSnapshot snapshot();
	static void write(const QString &strRole,
		const QString &strStage,
		quint64 nGeneration,
		bool bStale = false);

private:
	KResourceTraceLogger() = delete;
	~KResourceTraceLogger() = delete;
};

#endif // _WINREMOTECONTROL_COMMON_RESOURCETRACELOGGER_H_
