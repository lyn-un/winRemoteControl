#ifndef _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_

#include "adapters/clipboard/clipboardchangefilter.h"
#include "core/clipboard/clipboardadapter.h"

class QClipboard;

class KQtClipboardAdapter final : public KClipboardAdapter
{
	Q_OBJECT

public:
	explicit KQtClipboardAdapter(QObject *pParent = nullptr);

	QString text() const override;
	bool setText(const QString &strText, QString *pErrorMessage) override;

private:
	void handleClipboardChanged();
	void logIgnoredChange(const QString &strReason, quint32 nSequence) const;

	QClipboard *m_pClipboard = nullptr;
	KClipboardChangeFilter m_changeFilter;
};

#endif // _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_
