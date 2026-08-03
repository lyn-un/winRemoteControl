#ifndef _WINREMOTECONTROL_CORE_MEDIA_CAPTURESOURCE_H_
#define _WINREMOTECONTROL_CORE_MEDIA_CAPTURESOURCE_H_

#include "core/media/captureframe.h"

#include <QtCore/QString>

class IKCaptureSource
{
public:
	enum CaptureResult
	{
		CapturedCaptureResult,
		TimeoutCaptureResult,
		ErrorCaptureResult
	};

	virtual ~IKCaptureSource() = default;
	virtual bool initialize(QString *pErrorMessage) = 0;
	virtual CaptureResult captureNextFrame(KCaptureFrame *pFrame, QString *pErrorMessage) = 0;
	virtual void shutdown() = 0;
};

#endif // _WINREMOTECONTROL_CORE_MEDIA_CAPTURESOURCE_H_
