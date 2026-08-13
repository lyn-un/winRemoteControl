#include "core/terminal/terminalwriteall.h"

KTerminalWriteResult WriteAllTerminalData(
	const QByteArray &data,
	const KTerminalWriteFunction &writeFunction,
	const std::function<bool()> &shouldContinue)
{
	KTerminalWriteResult result;
	while (result.nBytesWritten < data.size() && shouldContinue())
	{
		qsizetype nWritten = 0;
		quint32 nErrorCode = 0;
		const qsizetype nRemaining = data.size() - result.nBytesWritten;
		if (!writeFunction(data.constData() + result.nBytesWritten,
			nRemaining, &nWritten, &nErrorCode))
		{
			result.nErrorCode = nErrorCode;
			return result;
		}
		if (nWritten <= 0 || nWritten > nRemaining)
		{
			result.nErrorCode = nErrorCode;
			return result;
		}
		result.nBytesWritten += nWritten;
	}
	result.bSucceeded = result.nBytesWritten == data.size();
	return result;
}
