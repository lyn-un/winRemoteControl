#ifndef _WINREMOTECONTROL_WEBRTCLATENCYPROBE_H_
#define _WINREMOTECONTROL_WEBRTCLATENCYPROBE_H_

#include "core/session/sessionstatemachine.h"

#include <QtCore/QString>

class KWebRtcLatencyProbe
{
public:
	QString createPing();
	bool handleMessage(const QString &strMessage,
		KSessionRole role,
		QString *pResponseMessage);
	void reset();
	int roundTripMs() const;

private:
	quint64 m_nPingId = 0;
	int m_nRoundTripMs = -1;
};

#endif // _WINREMOTECONTROL_WEBRTCLATENCYPROBE_H_
