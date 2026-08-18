#ifndef _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_
#define _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_

#include <QtCore/QMetaType>
#include <QtCore/QtGlobal>

#include <memory>
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
	bool bWebRtcLowLatencyRender = false;
	qint64 nRemoteCallbackAtMs = -1;
	qint64 nConversionDoneAtMs = -1;
	qint64 nRenderEnqueuedAtMs = -1;
	std::shared_ptr<std::vector<unsigned char>> spBgraBuffer;

	bool hasPixels() const
	{
		return spBgraBuffer != nullptr && !spBgraBuffer->empty();
	}

	const unsigned char *pixelData() const
	{
		return hasPixels() ? spBgraBuffer->data() : nullptr;
	}
};

Q_DECLARE_METATYPE(KDecodedVideoFrame)

#endif // _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_
