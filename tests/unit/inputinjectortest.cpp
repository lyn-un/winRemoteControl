#include "core/input/inputinjectorinterface.h"
#include "input/inputinjector.h"

#include <QtCore/QCoreApplication>

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
			++nInjectCount;
			return true;
		}

		void releaseAllKeys(QStringList *) override {}
		void releaseAllInputs(QStringList *) override { ++nReleaseCount; }

		KInputMessage lastMessage;
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
	return bSuccess ? 0 : 1;
}
