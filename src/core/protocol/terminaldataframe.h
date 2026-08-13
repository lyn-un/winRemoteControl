#ifndef _WINREMOTECONTROL_CORE_PROTOCOL_TERMINALDATAFRAME_H_
#define _WINREMOTECONTROL_CORE_PROTOCOL_TERMINALDATAFRAME_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>

enum KTerminalDataDirection
{
	InvalidTerminalDataDirection = 0,
	InputTerminalDataDirection = 1,
	OutputTerminalDataDirection = 2
};

struct KTerminalDataFrame
{
	KTerminalDataDirection direction = InvalidTerminalDataDirection;
	QString strRequestId;
	quint64 nSequence = 0;
	QByteArray payload;
};

class KTerminalDataFrameCodec
{
public:
	static constexpr qsizetype kHeaderBytes = 36;
	static constexpr qsizetype kMaximumFrameBytes = 64 * 1024;
	static constexpr qsizetype kMaximumPayloadBytes = kMaximumFrameBytes - kHeaderBytes;

	static QByteArray encode(const KTerminalDataFrame &frame,
		QString *pErrorMessage = nullptr);
	static bool decode(const QByteArray &data,
		KTerminalDataFrame *pFrame,
		QString *pErrorMessage = nullptr);
};

#endif // _WINREMOTECONTROL_CORE_PROTOCOL_TERMINALDATAFRAME_H_
