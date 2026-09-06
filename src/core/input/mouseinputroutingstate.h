#ifndef _WINREMOTECONTROL_MOUSEINPUTROUTINGSTATE_H_
#define _WINREMOTECONTROL_MOUSEINPUTROUTINGSTATE_H_

#include "core/protocol/inputmessage.h"

#include <QtCore/QSet>
#include <QtCore/QString>

struct KMouseInputRouteDecision
{
	bool bUseRealtimeChannel = false;
	bool bClearRealtimeQueue = false;
	QString strCoalescingKey;
};

class KMouseInputRoutingState
{
public:
	void setOrderedMouseDragEnabled(bool bEnabled);
	KMouseInputRouteDecision prepare(const KInputMessage &message,
		bool bRealtimeChannelAvailable);
	void reset();
	bool hasPressedButtons() const;

private:
	bool m_bOrderedMouseDragEnabled = false;
	quint64 m_nButtonStateEpoch = 0;
	QSet<int> m_pressedButtons;
};

#endif // _WINREMOTECONTROL_MOUSEINPUTROUTINGSTATE_H_
