#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALWRITEALL_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALWRITEALL_H_

#include <QtCore/QByteArray>

#include <functional>

struct KTerminalWriteResult
{
	bool bSucceeded = false;
	qsizetype nBytesWritten = 0;
	quint32 nErrorCode = 0;
};

using KTerminalWriteFunction = std::function<bool(const char *pData,
	qsizetype nBytes, qsizetype *pBytesWritten, quint32 *pErrorCode)>;

KTerminalWriteResult WriteAllTerminalData(const QByteArray &data,
	const KTerminalWriteFunction &writeFunction,
	const std::function<bool()> &shouldContinue);

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALWRITEALL_H_
