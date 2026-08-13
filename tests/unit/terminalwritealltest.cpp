#include "core/terminal/terminalwriteall.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (!bCondition)
		{
			qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
			++g_nFailureCount;
		}
	}

	void TestPartialWritesComplete()
	{
		QByteArray written;
		const QByteArray source("partial-write-data");
		const KTerminalWriteResult result = WriteAllTerminalData(source,
			[&written](const char *pData, qsizetype nBytes, qsizetype *pWritten,
				quint32 *pError)
			{
				const qsizetype nChunk = qMin<qsizetype>(3, nBytes);
				written.append(pData, nChunk);
				*pWritten = nChunk;
				*pError = 0;
				return true;
			}, []() { return true; });
		Check(result.bSucceeded && written == source,
			QStringLiteral("partial writes are completed without truncation"));
	}

	void TestZeroAndFailedWritesStop()
	{
		const KTerminalWriteResult zeroResult = WriteAllTerminalData(QByteArray("x"),
			[](const char *, qsizetype, qsizetype *pWritten, quint32 *pError)
			{
				*pWritten = 0;
				*pError = 0;
				return true;
			}, []() { return true; });
		Check(!zeroResult.bSucceeded,
			QStringLiteral("successful zero-byte write is rejected"));

		const KTerminalWriteResult failedResult = WriteAllTerminalData(QByteArray("x"),
			[](const char *, qsizetype, qsizetype *pWritten, quint32 *pError)
			{
				*pWritten = 0;
				*pError = 123;
				return false;
			}, []() { return true; });
		Check(!failedResult.bSucceeded && failedResult.nErrorCode == 123,
			QStringLiteral("writer failure preserves error code"));
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	TestPartialWritesComplete();
	TestZeroAndFailedWritesStop();
	return g_nFailureCount == 0 ? 0 : 1;
}
