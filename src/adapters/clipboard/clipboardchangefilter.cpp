#include "adapters/clipboard/clipboardchangefilter.h"

#include <QtCore/QMimeData>
#include <QtCore/QUrl>

bool KClipboardChangeFilter::hasFileData(const QMimeData &mimeData)
{
	if (mimeData.hasUrls())
	{
		const QList<QUrl> urls = mimeData.urls();
		for (const QUrl &url : urls)
		{
			if (url.isLocalFile())
				return true;
		}
	}

	const QStringList formats = mimeData.formats();
	for (const QString &strFormat : formats)
	{
		const QString strLowerFormat = strFormat.toLower();
		if (strLowerFormat.contains(QStringLiteral("filegroupdescriptor"))
			|| strLowerFormat.contains(QStringLiteral("filecontents"))
			|| strLowerFormat.contains(QStringLiteral("shell idlist array")))
		{
			return true;
		}
	}
	return false;
}

void KClipboardChangeFilter::prepareSelfWrite(const QString &strText)
{
	m_strSelfWrittenText = strText;
	m_bHasSelfWrite = true;
}

void KClipboardChangeFilter::confirmSelfWrite(quint32 nSequence)
{
	if (nSequence != 0)
		m_nLastSequence = nSequence;
}

void KClipboardChangeFilter::cancelSelfWrite()
{
	m_strSelfWrittenText.clear();
	m_bHasSelfWrite = false;
}

KClipboardChangeDecision KClipboardChangeFilter::evaluate(quint32 nSequence,
	bool bOwnsClipboard,
	const QString &strText,
	bool bHasFileData)
{
	if (bHasFileData)
	{
		if (nSequence != 0)
			m_nLastSequence = nSequence;
		cancelSelfWrite();
		return FileClipboardChangeDecision;
	}
	if (m_bHasSelfWrite && bOwnsClipboard && strText == m_strSelfWrittenText)
	{
		if (nSequence != 0)
			m_nLastSequence = nSequence;
		return SelfWriteClipboardChangeDecision;
	}
	if (nSequence != 0 && nSequence == m_nLastSequence)
		return DuplicateSequenceClipboardChangeDecision;

	if (nSequence != 0)
		m_nLastSequence = nSequence;
	cancelSelfWrite();
	return ForwardClipboardChangeDecision;
}
