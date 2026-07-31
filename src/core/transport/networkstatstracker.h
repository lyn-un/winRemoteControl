#ifndef _WINREMOTECONTROL_NETWORKSTATSTRACKER_H_
#define _WINREMOTECONTROL_NETWORKSTATSTRACKER_H_

#include "core/media/networkstats.h"

#include <QtCore/QtGlobal>

struct KNetworkStatsSample
{
	qint64 nTimestampMs = 0;
	quint64 nBytesReceived = 0;
	qint64 nPacketsReceived = 0;
	qint64 nPacketsLost = 0;
	int nRttMs = -1;
	int nJitterMs = -1;
	int nFps = 0;
	double fJitterBufferDelaySeconds = 0.0;
	double fJitterBufferTargetDelaySeconds = 0.0;
	quint64 nJitterBufferEmittedCount = 0;
	double fTotalDecodeTimeSeconds = 0.0;
	qint64 nFramesDecoded = 0;
	qint64 nKeyFramesDecoded = 0;
	qint64 nFramesDropped = 0;
	bool bHasInboundVideo = false;
};

class KNetworkStatsTracker
{
public:
	KNetworkStats update(const KNetworkStatsSample &sample, int nDataChannelRttMs);
	void reset();

private:
	static QString evaluateQuality(const KNetworkStats &stats);

	bool m_bHasPreviousSample = false;
	KNetworkStatsSample m_previousSample;
};

#endif // _WINREMOTECONTROL_NETWORKSTATSTRACKER_H_
