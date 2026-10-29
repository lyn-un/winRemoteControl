#ifndef _WINREMOTECONTROL_CORE_MEDIA_CAPTUREFRAME_H_
#define _WINREMOTECONTROL_CORE_MEDIA_CAPTUREFRAME_H_

#include <QtCore/QtGlobal>

#include <vector>
#include <memory>

#include "core/media/nativevideoframe.h"

struct KCaptureFrame
{
	int nWidth = 0;
	int nHeight = 0;
	quint64 nFrameIndex = 0;
	qint64 nTimestampMs = 0;
	std::vector<unsigned char> vecBgraBuffer;
	std::shared_ptr<IKNativeVideoFrame> spNativeFrame;
};

#endif // _WINREMOTECONTROL_CORE_MEDIA_CAPTUREFRAME_H_
