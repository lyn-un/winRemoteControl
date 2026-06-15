#ifndef _WINREMOTECONTROL_WEBRTCVIDEOFRAME_H_
#define _WINREMOTECONTROL_WEBRTCVIDEOFRAME_H_

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>

struct KWebRtcVideoFrame
{
	int nWidth = 0;
	int nHeight = 0;
	quint64 nFrameIndex = 0;
	qint64 nTimestampMs = 0;
	quint64 nLastInputSeq = 0;
	qint64 nInputAgeMs = -1;
	QByteArray yPlane;
	QByteArray uPlane;
	QByteArray vPlane;
	int nStrideY = 0;
	int nStrideU = 0;
	int nStrideV = 0;
};

Q_DECLARE_METATYPE(KWebRtcVideoFrame)

#endif // _WINREMOTECONTROL_WEBRTCVIDEOFRAME_H_
