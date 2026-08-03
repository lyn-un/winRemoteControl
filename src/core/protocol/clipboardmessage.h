#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_CLIPBOARDMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_CLIPBOARDMESSAGE_H_

#include <QtCore/QString>
#include <QtCore/QMetaType>

enum KClipboardMessageType
{
	InvalidClipboardMessageType,
	ReadyClipboardMessageType,
	TextClipboardMessageType,
	SyncStateClipboardMessageType
};

struct KClipboardMessage
{
	KClipboardMessageType type = InvalidClipboardMessageType;
	QString strMessageId;
	QString strText;
	bool bEnabled = false;
};

Q_DECLARE_METATYPE(KClipboardMessage)

class KClipboardMessageCodec
{
public:
	static constexpr int kProtocolVersion = 1;
	static constexpr int kMaximumTextBytes = 256000;

	static QString encode(const KClipboardMessage &message);
	static bool decode(const QString &strMessage,
		KClipboardMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KClipboardMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_CLIPBOARDMESSAGE_H_
