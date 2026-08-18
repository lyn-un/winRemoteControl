#include "session/inputfeedbacktracker.h"

#include "common/latencytracelogger.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <iostream>

namespace
{
bool Check(bool bCondition, const char *pDescription)
{
	if (bCondition)
		return true;
	std::cerr << "FAILED: " << pDescription << '\n';
	return false;
}
}

int main(int nArgumentCount, char **pArguments)
{
	QCoreApplication application(nArgumentCount, pArguments);
	QTemporaryDir temporaryDirectory;
	if (!Check(temporaryDirectory.isValid(),
			"temporary trace directory is available"))
	{
		return 1;
	}

	KLatencyTraceLogger::configure(true, temporaryDirectory.path(), QString());
	KInputFeedbackTracker tracker;

	KInputMessage keyRelease;
	keyRelease.type = KeyInputMessageType;
	keyRelease.nSequence = 1;
	keyRelease.bPressed = false;
	tracker.recordInputSent(keyRelease);
	tracker.handleRendered(keyRelease.nSequence);

	KInputMessage mouseMove;
	mouseMove.type = MouseMoveInputMessageType;
	mouseMove.nSequence = 2;
	tracker.recordInputSent(mouseMove);
	tracker.handleRendered(mouseMove.nSequence);
	tracker.reset();
	KLatencyTraceLogger::shutdown();

	QFile traceFile(QDir(temporaryDirectory.path()).absoluteFilePath(
		QStringLiteral("latency_trace_controller.log")));
	if (!Check(traceFile.open(QIODevice::ReadOnly | QIODevice::Text),
			"controller latency trace is readable"))
	{
		return 1;
	}
	const QByteArray trace = traceFile.readAll();
	if (!Check(trace.contains("stage=input_roundtrip_summary"),
			"session reset emits a final round-trip summary"))
	{
		return 1;
	}
	if (!Check(trace.contains("samples=1 totalSamples=1"),
			"key release is excluded from the final round-trip sample set"))
	{
		return 1;
	}
	return 0;
}
