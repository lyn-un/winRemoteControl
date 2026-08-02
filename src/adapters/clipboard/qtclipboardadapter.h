#ifndef _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_
#define _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_

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

	QClipboard *m_pClipboard = nullptr;
};

#endif // _WINREMOTECONTROL_ADAPTERS_CLIPBOARD_QTCLIPBOARDADAPTER_H_
