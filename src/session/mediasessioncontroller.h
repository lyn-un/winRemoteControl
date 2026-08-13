#ifndef _WINREMOTECONTROL_SESSION_MEDIASESSIONCONTROLLER_H_
#define _WINREMOTECONTROL_SESSION_MEDIASESSIONCONTROLLER_H_

#include "core/media/streamconfig.h"
#include "core/protocol/sessionmessage.h"

#include <QtCore/QObject>

class KMediaSessionController final : public QObject
{
	Q_OBJECT

public:
	explicit KMediaSessionController(QObject *pParent = nullptr);
	void setCapabilities(const KNegotiatedCapabilities &capabilities);
	KStreamConfig constrainedConfig(const KStreamConfig &config) const;
	void startCapture(quint64 nGeneration);
	void stopCapture(quint64 nGeneration);
	void reset();
	bool isCaptureActive() const;

signals:
	void startCaptureRequested(quint64 nGeneration);
	void stopCaptureRequested(quint64 nGeneration);

private:
	KNegotiatedCapabilities m_capabilities;
	bool m_bCaptureActive = false;
};

#endif // _WINREMOTECONTROL_SESSION_MEDIASESSIONCONTROLLER_H_
