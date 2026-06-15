#ifndef _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_
#define _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_

#include <QtCore/QMetaType>
#include <QtCore/QtGlobal>

#include <vector>

struct KDecodedVideoFrame
{
	int nWidth = 0;
	int nHeight = 0;
	quint64 nFrameIndex = 0;
	quint64 nSourceFrameIndex = 0;
	qint64 nTimestampMs = 0;
	quint64 nLastInputSeq = 0;
	qint64 nInputAgeMs = -1;
	std::vector<unsigned char> vecBgraBuffer;
};

Q_DECLARE_METATYPE(KDecodedVideoFrame)

#endif // _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_
