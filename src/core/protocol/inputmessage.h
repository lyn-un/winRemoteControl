#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_INPUTMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_INPUTMESSAGE_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

enum KInputMessageType
{
	InvalidInputMessageType,
	MouseMoveInputMessageType,
	MouseButtonInputMessageType,
	MouseWheelInputMessageType,
	KeyInputMessageType
};

enum KRemoteMouseButton
{
	NoRemoteMouseButton,
	LeftRemoteMouseButton,
	RightRemoteMouseButton
};

struct KInputMessage
{
	KInputMessageType type = InvalidInputMessageType;
	KRemoteMouseButton mouseButton = NoRemoteMouseButton;
	int nX = 0;
	int nY = 0;
	int nWheelDelta = 0;
	int nVirtualKey = 0;
	bool bPressed = false;
	bool bExtended = false;
	bool bTrace = false;
	quint64 nSequence = 0;
};

class KInputMessageCodec
{
public:
	static QString encode(const KInputMessage &message);
	static bool decode(const QString &strMessage, KInputMessage *pMessage, QString *pErrorMessage);
	static QString typeName(KInputMessageType type);
	static QString mouseButtonName(KRemoteMouseButton button);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_INPUTMESSAGE_H_
