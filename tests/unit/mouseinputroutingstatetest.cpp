#include "core/input/mouseinputroutingstate.h"

#include <QtCore/QCoreApplication>

#include <iostream>

namespace
{
	bool Require(bool bCondition, const char *pMessage)
	{
		if (bCondition)
			return true;
		std::cerr << pMessage << std::endl;
		return false;
	}

	KInputMessage MouseButton(KRemoteMouseButton button, bool bPressed)
	{
		KInputMessage message;
		message.type = MouseButtonInputMessageType;
		message.mouseButton = button;
		message.bPressed = bPressed;
		return message;
	}

	KInputMessage MouseMove()
	{
		KInputMessage message;
		message.type = MouseMoveInputMessageType;
		return message;
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	KMouseInputRoutingState state;
	state.setOrderedMouseDragEnabled(true);

	bool bSuccess = true;
	KMouseInputRouteDecision decision = state.prepare(MouseMove(), true);
	bSuccess &= Require(decision.bUseRealtimeChannel,
		"idle mouse movement did not use the realtime channel");

	decision = state.prepare(MouseButton(LeftRemoteMouseButton, true), true);
	bSuccess &= Require(decision.bClearRealtimeQueue && state.hasPressedButtons(),
		"mouse down did not start an ordered drag interval");
	const KMouseInputRouteDecision firstDragMove = state.prepare(MouseMove(), true);
	const KMouseInputRouteDecision secondDragMove = state.prepare(MouseMove(), true);
	bSuccess &= Require(!firstDragMove.bUseRealtimeChannel
			&& !firstDragMove.strCoalescingKey.isEmpty()
			&& firstDragMove.strCoalescingKey == secondDragMove.strCoalescingKey,
		"drag movement was not routed and coalesced on the reliable channel");
	KInputMessage keyMessage;
	keyMessage.type = KeyInputMessageType;
	state.prepare(keyMessage, true);
	const KMouseInputRouteDecision moveAfterKey = state.prepare(MouseMove(), true);
	bSuccess &= Require(moveAfterKey.strCoalescingKey
			!= secondDragMove.strCoalescingKey,
		"drag movement was coalesced across a non-move input barrier");

	state.prepare(MouseButton(RightRemoteMouseButton, true), true);
	const KMouseInputRouteDecision multiButtonMove = state.prepare(MouseMove(), true);
	bSuccess &= Require(multiButtonMove.strCoalescingKey
			!= firstDragMove.strCoalescingKey,
		"a button transition did not create a queue ordering barrier");
	state.prepare(MouseButton(LeftRemoteMouseButton, false), true);
	bSuccess &= Require(!state.prepare(MouseMove(), true).bUseRealtimeChannel,
		"releasing one of two buttons ended ordered drag routing");
	state.prepare(MouseButton(RightRemoteMouseButton, false), true);
	bSuccess &= Require(state.prepare(MouseMove(), true).bUseRealtimeChannel,
		"releasing the final button did not restore realtime routing");

	state.setOrderedMouseDragEnabled(false);
	state.prepare(MouseButton(LeftRemoteMouseButton, true), true);
	bSuccess &= Require(state.prepare(MouseMove(), true).bUseRealtimeChannel,
		"legacy routing changed when ordered drag was not negotiated");
	state.reset();
	bSuccess &= Require(!state.hasPressedButtons(),
		"routing reset retained pressed mouse buttons");
	return bSuccess ? 0 : 1;
}
