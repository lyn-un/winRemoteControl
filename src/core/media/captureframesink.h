#ifndef _WINREMOTECONTROL_CORE_MEDIA_CAPTUREFRAMESINK_H_
#define _WINREMOTECONTROL_CORE_MEDIA_CAPTUREFRAMESINK_H_

#include "core/media/captureframe.h"
#include "core/media/streamconfig.h"

#include <QtCore/QString>

class IKCaptureFrameSink
{
public:
	virtual ~IKCaptureFrameSink() = default;
	virtual bool initialize(QString *pErrorMessage) = 0;
	virtual bool processFrame(KCaptureFrame frame, QString *pErrorMessage) = 0;
	virtual void handleCaptureTimeout() = 0;
	virtual void shutdown() = 0;
	virtual void setStreamConfig(const KStreamConfig &config) = 0;
	virtual void setInputTraceState(quint64 nSeq, qint64 nInjectedMs) = 0;
};

#endif // _WINREMOTECONTROL_CORE_MEDIA_CAPTUREFRAMESINK_H_
