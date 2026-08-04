#ifndef _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_

#include "adapters/clipboard/clipboardchangefilter.h"
#include "core/clipboard/clipboardadapter.h"

class QClipboard;
class QTimer;

class KQtClipboardAdapter final : public KClipboardAdapter
{
	Q_OBJECT

public:
	explicit KQtClipboardAdapter(QObject *pParent = nullptr);

	QString text() const override;
	bool setText(const QString &strText, QString *pErrorMessage) override;

private:
	void handleClipboardChanged();
	void processPendingClipboardChange();
	void logIgnoredChange(const QString &strReason, quint32 nSequence) const;

	QClipboard *m_pClipboard = nullptr;
	QTimer *m_pReadTimer = nullptr;
	KClipboardChangeFilter m_changeFilter;
	int m_nReadRetryCount = 0;
};

#endif // _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_
