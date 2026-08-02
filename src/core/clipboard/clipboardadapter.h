#ifndef _WINREMOTECONTROL_CORE_CLIPBOARD_CLIPBOARDADAPTER_H_
#define _WINREMOTECONTROL_CORE_CLIPBOARD_CLIPBOARDADAPTER_H_

#include <QtCore/QObject>
#include <QtCore/QString>

class KClipboardAdapter : public QObject
{
	Q_OBJECT

public:
	explicit KClipboardAdapter(QObject *pParent = nullptr)
		: QObject(pParent)
	{
	}

	~KClipboardAdapter() override = default;

	virtual QString text() const = 0;
	virtual bool setText(const QString &strText, QString *pErrorMessage) = 0;

signals:
	void textChanged(const QString &strText);
};

#endif // _WINREMOTECONTROL_CORE_CLIPBOARD_CLIPBOARDADAPTER_H_
