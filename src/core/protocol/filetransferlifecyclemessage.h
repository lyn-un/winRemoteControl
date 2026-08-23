#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERLIFECYCLEMESSAGE_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERLIFECYCLEMESSAGE_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

struct KProtocolEnvelope;

enum KFileTransferLifecycleMessageType
{
	InvalidFileTransferLifecycleMessageType,
	OpenRequestFileTransferLifecycleMessageType,
	OpenAcceptedFileTransferLifecycleMessageType,
	OpenRejectedFileTransferLifecycleMessageType,
	CloseFileTransferLifecycleMessageType,
	StoppedFileTransferLifecycleMessageType,
	ErrorFileTransferLifecycleMessageType
};

struct KFileTransferLifecycleMessage
{
	KFileTransferLifecycleMessageType type = InvalidFileTransferLifecycleMessageType;
	QString strRequestId;
	quint64 nGeneration = 0;
	QString strErrorCode;
};

class KFileTransferLifecycleMessageCodec
{
public:
	static QString encode(const KFileTransferLifecycleMessage &message);
	static bool decode(const QString &strMessage,
		KFileTransferLifecycleMessage *pMessage,
		QString *pErrorMessage);
	static bool decode(const KProtocolEnvelope &envelope,
		KFileTransferLifecycleMessage *pMessage,
		QString *pErrorMessage);
	static QString typeName(KFileTransferLifecycleMessageType type);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERLIFECYCLEMESSAGE_H_
