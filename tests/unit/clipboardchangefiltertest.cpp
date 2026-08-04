#include "adapters/clipboard/clipboardchangefilter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>

#include <iostream>

namespace
{
	int g_nFailureCount = 0;

	void Check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		std::cerr << "FAILED: " << strDescription.toStdString() << '\n';
		++g_nFailureCount;
	}

	void TestSelfWriteAndSequenceFiltering()
	{
		KClipboardChangeFilter filter;
		filter.prepareSelfWrite(QStringLiteral("remote text"));
		Check(filter.evaluate(10, true, QStringLiteral("remote text"), false)
			== SelfWriteClipboardChangeDecision,
			QStringLiteral("first self-write notification is ignored"));
		Check(filter.evaluate(10, false, QStringLiteral("remote text"), false)
			== DuplicateSequenceClipboardChangeDecision,
			QStringLiteral("repeated self-write sequence remains ignored without ownership"));
		Check(filter.evaluate(11, false, QStringLiteral("remote text"), false)
			== ForwardClipboardChangeDecision,
			QStringLiteral("same text copied later by another application is forwarded"));
		Check(filter.evaluate(11, false, QStringLiteral("remote text"), false)
			== DuplicateSequenceClipboardChangeDecision,
			QStringLiteral("duplicate sequence notification is ignored"));

		filter.prepareSelfWrite(QStringLiteral("fallback"));
		Check(filter.evaluate(0, true, QStringLiteral("fallback"), false)
			== SelfWriteClipboardChangeDecision,
			QStringLiteral("clipboard ownership suppresses self writes without a sequence"));
		Check(filter.evaluate(0, false, QStringLiteral("fallback"), false)
			== ForwardClipboardChangeDecision,
			QStringLiteral("external changes still forward when sequence is unavailable"));
	}

	void TestFileClipboardDetection()
	{
		QMimeData plainText;
		plainText.setText(QStringLiteral("中文 Emoji 😀"));
		Check(!KClipboardChangeFilter::hasFileData(plainText),
			QStringLiteral("plain text is not classified as a file"));

		QMimeData webLink;
		webLink.setUrls({QUrl(QStringLiteral("https://example.com/path"))});
		Check(!KClipboardChangeFilter::hasFileData(webLink),
			QStringLiteral("web links remain eligible for text synchronization"));

		QMimeData localFile;
		localFile.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/first.txt"))});
		Check(KClipboardChangeFilter::hasFileData(localFile),
			QStringLiteral("a single local file URL is rejected"));

		QMimeData localFiles;
		localFiles.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/first.txt")),
			QUrl::fromLocalFile(QStringLiteral("C:/second.txt"))});
		Check(KClipboardChangeFilter::hasFileData(localFiles),
			QStringLiteral("multiple local file URLs are rejected"));

		QMimeData virtualFile;
		virtualFile.setData(
			QStringLiteral("application/x-qt-windows-mime;value=\"FileGroupDescriptorW\""),
			QByteArrayLiteral("descriptor"));
		Check(KClipboardChangeFilter::hasFileData(virtualFile),
			QStringLiteral("Windows file descriptors are rejected"));

		QMimeData virtualFileContents;
		virtualFileContents.setData(
			QStringLiteral("application/x-qt-windows-mime;value=\"FileContents\";index=0"),
			QByteArrayLiteral("contents"));
		Check(KClipboardChangeFilter::hasFileData(virtualFileContents),
			QStringLiteral("Windows virtual file contents are rejected"));

		QMimeData shellFileList;
		shellFileList.setData(
			QStringLiteral("application/x-qt-windows-mime;value=\"Shell IDList Array\""),
			QByteArrayLiteral("shell-list"));
		Check(KClipboardChangeFilter::hasFileData(shellFileList),
			QStringLiteral("Windows shell file lists are rejected"));

		KClipboardChangeFilter filter;
		Check(filter.evaluate(20, false, QStringLiteral("C:/first.txt"), true)
			== FileClipboardChangeDecision,
			QStringLiteral("file clipboard changes are not forwarded"));
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	TestSelfWriteAndSequenceFiltering();
	TestFileClipboardDetection();
	if (g_nFailureCount == 0)
		qInfo() << "All clipboard change filter tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
