#ifndef _WINREMOTECONTROL_INPUTFEEDBACKTRACKER_H_
#define _WINREMOTECONTROL_INPUTFEEDBACKTRACKER_H_

#include "core/protocol/inputmessage.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QVector>

class KInputFeedbackTracker
{
public:
	KInputFeedbackTracker() = default;

	KInputFeedbackTracker(const KInputFeedbackTracker &) = delete;
	KInputFeedbackTracker &operator=(const KInputFeedbackTracker &) = delete;

	bool shouldTraceMouseMove();
	void recordInputSent(const KInputMessage &message);
	void handleRendered(quint64 nSeq);
	void reset();

private:
	struct KPendingInputTrace
	{
		qint64 nSentMs = 0;
		QString strType;
		bool bKeyPressed = false;
	};

	void logRoundTripStats(const QString &strEventName);

	quint64 m_nRoundTripSampleCount = 0;
	QElapsedTimer m_moveTraceTimer;
	QElapsedTimer m_roundTripTimer;
	QMap<quint64, KPendingInputTrace> m_sentTraces;
	QVector<qint64> m_roundTripSamples;
};

#endif // _WINREMOTECONTROL_INPUTFEEDBACKTRACKER_H_
