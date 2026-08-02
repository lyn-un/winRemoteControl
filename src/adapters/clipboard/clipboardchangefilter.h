#ifndef _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_CLIPBOARDCHANGEFILTER_H_
#define _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_CLIPBOARDCHANGEFILTER_H_

#include <QtCore/QString>
#include <QtCore/QtGlobal>

class QMimeData;

enum KClipboardChangeDecision
{
	ForwardClipboardChangeDecision,
	SelfWriteClipboardChangeDecision,
	DuplicateSequenceClipboardChangeDecision,
	FileClipboardChangeDecision
};

class KClipboardChangeFilter
{
public:
	static bool hasFileData(const QMimeData &mimeData);

	void prepareSelfWrite(const QString &strText);
	void confirmSelfWrite(quint32 nSequence);
	void cancelSelfWrite();
	KClipboardChangeDecision evaluate(quint32 nSequence,
		bool bOwnsClipboard,
		const QString &strText,
		bool bHasFileData);

private:
	QString m_strSelfWrittenText;
	quint32 m_nLastSequence = 0;
	bool m_bHasSelfWrite = false;
};

#endif // _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_CLIPBOARDCHANGEFILTER_H_
