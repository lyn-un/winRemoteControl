#ifndef _WINREMOTECONTROL_WEBRTCNETWORKSTATS_H_
#define _WINREMOTECONTROL_WEBRTCNETWORKSTATS_H_

#include <QtCore/QMetaType>
#include <QtCore/QString>

struct KWebRtcNetworkStats
{
	int nRttMs = -1;
	int nJitterMs = -1;
	double fPacketLossRate = 0.0;
	int nBitrateKbps = 0;
	int nFps = 0;
	int nDataChannelRttMs = -1;
	int nJitterBufferDelayMs = -1;
	int nJitterBufferTargetDelayMs = -1;
	int nDecodeTimeMs = -1;
	int nFramesDecoded = 0;
	int nKeyFramesDecoded = 0;
	int nFramesDropped = 0;
	QString strQuality = QStringLiteral("unknown");
};

Q_DECLARE_METATYPE(KWebRtcNetworkStats)

#endif // _WINREMOTECONTROL_WEBRTCNETWORKSTATS_H_
