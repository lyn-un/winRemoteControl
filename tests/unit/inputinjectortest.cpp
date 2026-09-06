#include "core/input/inputinjectorinterface.h"
#include "input/inputinjector.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QList>

#include <iostream>
#include <memory>

namespace
{
	class KFakePlatformInputInjector : public IKInputInjector
	{
	public:
		bool inject(const KInputMessage &message, QString *) override
		{
			lastMessage = message;
			messages.append(message);
			++nInjectCount;
			return true;
		}

		void releaseAllKeys(QStringList *) override {}
		void releaseAllInputs(QStringList *) override { ++nReleaseCount; }

		KInputMessage lastMessage;
		QList<KInputMessage> messages;
		int nInjectCount = 0;
		int nReleaseCount = 0;
	};

	bool require(bool bCondition, const char *pMessage)
	{
		if (bCondition)
			return true;
		std::cerr << pMessage << std::endl;
		return false;
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	auto upPlatformInjector = std::make_unique<KFakePlatformInputInjector>();
	KFakePlatformInputInjector *pPlatformInjector = upPlatformInjector.get();
	KInputInjector injector(std::move(upPlatformInjector));

	KInputMessage message;
	message.type = KeyInputMessageType;
	message.nVirtualKey = 65;
	message.bPressed = true;
	message.nSequence = 2;
	injector.handleInputMessage(message);
	injector.handleInputMessage(message);
	message.nSequence = 1;
	injector.handleInputMessage(message);

	bool bSuccess = require(pPlatformInjector->nInjectCount == 1,
		"duplicate and descending input sequence was not discarded");

	KInputMessage pointerMessage;
	pointerMessage.type = MouseMoveInputMessageType;
	pointerMessage.nSequence = 100;
	injector.handleInputMessage(pointerMessage);
	message.nSequence = 3;
	injector.handleInputMessage(message);
	bSuccess &= require(pPlatformInjector->nInjectCount == 3,
		"a larger pointer sequence discarded a reliable key event");
	pointerMessage.nSequence = 102;
	injector.handleInputMessage(pointerMessage);
	pointerMessage.nSequence = 101;
	injector.handleInputMessage(pointerMessage);
	bSuccess &= require(pPlatformInjector->nInjectCount == 4,
		"out-of-order pointer input did not keep only the latest sequence");

	injector.releaseAllInputs();
	message.nSequence = 1;
	injector.handleInputMessage(message);
	bSuccess &= require(pPlatformInjector->nInjectCount == 5,
		"input sequence did not reset after releasing the session");

	const int nMessagesBeforeDoubleClick = pPlatformInjector->messages.size();
	for (int i = 0; i < 4; ++i)
	{
		KInputMessage mouseButtonMessage;
		mouseButtonMessage.type = MouseButtonInputMessageType;
		mouseButtonMessage.mouseButton = LeftRemoteMouseButton;
		mouseButtonMessage.bPressed = i == 0 || i == 2;
		mouseButtonMessage.nX = 640;
		mouseButtonMessage.nY = 360;
		mouseButtonMessage.nSequence = static_cast<quint64>(3 + i * 2);
		injector.handleInputMessage(mouseButtonMessage);
	}
	const QList<KInputMessage> doubleClickMessages =
		pPlatformInjector->messages.mid(nMessagesBeforeDoubleClick);
	bSuccess &= require(doubleClickMessages.size() == 4,
		"double click did not preserve all four reliable input messages");
	for (int i = 0; i < doubleClickMessages.size(); ++i)
	{
		const KInputMessage &mouseButtonMessage = doubleClickMessages.at(i);
		bSuccess &= require(mouseButtonMessage.type == MouseButtonInputMessageType
				&& mouseButtonMessage.mouseButton == LeftRemoteMouseButton
				&& mouseButtonMessage.bPressed == (i == 0 || i == 2)
				&& mouseButtonMessage.nX == 640
				&& mouseButtonMessage.nY == 360,
			"double click message order or payload changed before platform injection");
	}

	injector.releaseAllInputs();
	injector.setOrderedMouseDragEnabled(true);
	const int nMessagesBeforeOrderedDrag = pPlatformInjector->messages.size();
	KInputMessage leftDown;
	leftDown.type = MouseButtonInputMessageType;
	leftDown.mouseButton = LeftRemoteMouseButton;
	leftDown.bPressed = true;
	leftDown.nSequence = 1;
	injector.handleInputMessage(leftDown);

	KInputMessage lateRealtimeMove;
	lateRealtimeMove.type = MouseMoveInputMessageType;
	lateRealtimeMove.bRealtime = true;
	lateRealtimeMove.nSequence = 2;
	injector.handleInputMessage(lateRealtimeMove);
	KInputMessage reliableDragMove = lateRealtimeMove;
	reliableDragMove.bRealtime = false;
	reliableDragMove.nSequence = 4;
	injector.handleInputMessage(reliableDragMove);
	KInputMessage leftUp = leftDown;
	leftUp.bPressed = false;
	leftUp.nSequence = 3;
	injector.handleInputMessage(leftUp);
	KInputMessage realtimeMoveAfterRelease = lateRealtimeMove;
	realtimeMoveAfterRelease.nSequence = 6;
	injector.handleInputMessage(realtimeMoveAfterRelease);

	const QList<KInputMessage> orderedDragMessages =
		pPlatformInjector->messages.mid(nMessagesBeforeOrderedDrag);
	bSuccess &= require(orderedDragMessages.size() == 4
			&& orderedDragMessages.at(0).type == MouseButtonInputMessageType
			&& orderedDragMessages.at(0).bPressed
			&& orderedDragMessages.at(1).type == MouseMoveInputMessageType
			&& !orderedDragMessages.at(1).bRealtime
			&& orderedDragMessages.at(2).type == MouseButtonInputMessageType
			&& !orderedDragMessages.at(2).bPressed
			&& orderedDragMessages.at(3).type == MouseMoveInputMessageType
			&& orderedDragMessages.at(3).bRealtime,
		"ordered drag did not drop only the late realtime move while pressed");

	injector.releaseAllInputs();
	const int nMessagesBeforeSequenceWatermark = pPlatformInjector->messages.size();
	leftDown.nSequence = 1;
	injector.handleInputMessage(leftDown);
	lateRealtimeMove.nSequence = 10;
	injector.handleInputMessage(lateRealtimeMove);
	leftUp.nSequence = 3;
	injector.handleInputMessage(leftUp);
	realtimeMoveAfterRelease.nSequence = 8;
	injector.handleInputMessage(realtimeMoveAfterRelease);
	realtimeMoveAfterRelease.nSequence = 12;
	injector.handleInputMessage(realtimeMoveAfterRelease);
	bSuccess &= require(pPlatformInjector->messages.size()
			== nMessagesBeforeSequenceWatermark + 3,
		"a dropped realtime move did not advance the pointer sequence watermark");

	injector.releaseAllInputs();
	injector.setOrderedMouseDragEnabled(false);
	const int nMessagesBeforeLegacyRoute = pPlatformInjector->messages.size();
	leftDown.nSequence = 1;
	injector.handleInputMessage(leftDown);
	lateRealtimeMove.nSequence = 2;
	injector.handleInputMessage(lateRealtimeMove);
	bSuccess &= require(pPlatformInjector->messages.size()
			== nMessagesBeforeLegacyRoute + 2,
		"legacy peer realtime movement changed without capability negotiation");
	return bSuccess ? 0 : 1;
}
