#ifndef _WINREMOTECONTROL_COMMON_TRACEOPTIONS_H_
#define _WINREMOTECONTROL_COMMON_TRACEOPTIONS_H_

#include <QtCore/QString>

class QCommandLineParser;

struct KTraceOptions
{
	bool bSessionTraceEnabled = false;
	bool bLatencyTraceEnabled = false;
	QString strLogDirectory;
	QString strLatencyScenario;
	QString strValidationError;
};

class KTraceOptionsParser
{
public:
	static void addOptions(QCommandLineParser *pParser);
	static KTraceOptions options(const QCommandLineParser &parser,
		const QString &strApplicationDirectory);

private:
	KTraceOptionsParser() = delete;
	~KTraceOptionsParser() = delete;
};

#endif // _WINREMOTECONTROL_COMMON_TRACEOPTIONS_H_
