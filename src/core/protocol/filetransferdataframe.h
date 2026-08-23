#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERDATAFRAME_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERDATAFRAME_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

struct KFileTransferDataFrame
{
	quint16 nFlags = 0;
	QString strTaskId;
	QString strFileId;
	quint64 nOffset = 0;
	QByteArray payload;
};

class KFileTransferDataFrameCodec
{
public:
	static constexpr qsizetype kHeaderBytes = 52;
	static constexpr qsizetype kMaximumFrameBytes = 64 * 1024;
	static constexpr qsizetype kMaximumPayloadBytes = kMaximumFrameBytes - kHeaderBytes;

	static QByteArray encode(const KFileTransferDataFrame &frame,
		QString *pErrorMessage = nullptr);
	static bool decode(const QByteArray &data,
		KFileTransferDataFrame *pFrame,
		QString *pErrorMessage = nullptr);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_FILETRANSFERDATAFRAME_H_
