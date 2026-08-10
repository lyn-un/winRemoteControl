#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYFRAMECODEC_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYFRAMECODEC_H_

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

struct KTerminalRelayFrame
{
	quint16 nType = 0;
	QByteArray payload;
};

class KTerminalRelayFrameCodec
{
public:
	static QByteArray encode(quint16 nType, const QByteArray &payload = QByteArray());
	bool append(const QByteArray &data, QVector<KTerminalRelayFrame> *pFrames,
		QString *pErrorMessage = nullptr);
	void clear();

private:
	QByteArray m_buffer;
};

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYFRAMECODEC_H_
