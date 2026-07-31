#ifndef _WINREMOTECONTROL_CAPTURECONTROLLER_H_
#define _WINREMOTECONTROL_CAPTURECONTROLLER_H_

#include "core/media/decodedvideoframe.h"

#include <QtCore/QObject>
#include <QtCore/QString>

class KCaptureController : public QObject
{
	Q_OBJECT

public:
	explicit KCaptureController(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KCaptureController() override = default;

	KCaptureController(const KCaptureController &) = delete;
	KCaptureController &operator=(const KCaptureController &) = delete;

public slots:
	virtual void startCapture() = 0;
	virtual void stopCapture() = 0;

signals:
	void statusChanged(const QString &strStatus);
	void captureError(const QString &strMessage);
	void decodedFrameReady(const KDecodedVideoFrame &frame);
	void frameReady(int nWidth, int nHeight, quint64 nFrameIndex, qint64 nTimestampMs);
};

#endif // _WINREMOTECONTROL_CAPTURECONTROLLER_H_
