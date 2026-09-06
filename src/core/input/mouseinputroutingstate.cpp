#include "core/input/mouseinputroutingstate.h"

void KMouseInputRoutingState::setOrderedMouseDragEnabled(bool bEnabled)
{
	reset();
	m_bOrderedMouseDragEnabled = bEnabled;
}

KMouseInputRouteDecision KMouseInputRoutingState::prepare(
	const KInputMessage &message,
	bool bRealtimeChannelAvailable)
{
	KMouseInputRouteDecision decision;
	if (message.type == MouseButtonInputMessageType)
	{
		++m_nButtonStateEpoch;
		decision.bClearRealtimeQueue = m_bOrderedMouseDragEnabled
			&& message.bPressed;
		if (message.bPressed)
			m_pressedButtons.insert(static_cast<int>(message.mouseButton));
		else
			m_pressedButtons.remove(static_cast<int>(message.mouseButton));
		return decision;
	}

	if (message.type != MouseMoveInputMessageType)
	{
		if (m_bOrderedMouseDragEnabled && !m_pressedButtons.isEmpty())
			++m_nButtonStateEpoch;
		return decision;
	}

	decision.bUseRealtimeChannel = bRealtimeChannelAvailable
		&& (!m_bOrderedMouseDragEnabled || m_pressedButtons.isEmpty());
	if (m_bOrderedMouseDragEnabled && !m_pressedButtons.isEmpty())
	{
		decision.strCoalescingKey = QStringLiteral("dragMouseMove:%1")
			.arg(m_nButtonStateEpoch);
	}
	else
	{
		decision.strCoalescingKey = QStringLiteral("mouseMove");
	}
	return decision;
}

void KMouseInputRoutingState::reset()
{
	m_pressedButtons.clear();
	m_nButtonStateEpoch = 0;
}

bool KMouseInputRoutingState::hasPressedButtons() const
{
	return !m_pressedButtons.isEmpty();
}
