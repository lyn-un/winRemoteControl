#include "adapters/clipboard/qtclipboardadapter.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QMimeData>

KQtClipboardAdapter::KQtClipboardAdapter(QObject *pParent)
	: KClipboardAdapter(pParent)
	, m_pClipboard(QGuiApplication::clipboard())
{
	connect(m_pClipboard, &QClipboard::dataChanged,
		this, &KQtClipboardAdapter::handleClipboardChanged);
}

QString KQtClipboardAdapter::text() const
{
	const QMimeData *pMimeData = m_pClipboard->mimeData(QClipboard::Clipboard);
	return pMimeData != nullptr && pMimeData->hasText()
		? pMimeData->text()
		: QString();
}

bool KQtClipboardAdapter::setText(const QString &strText, QString *pErrorMessage)
{
	m_pClipboard->setText(strText, QClipboard::Clipboard);
	if (text() != strText)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("无法写入系统剪贴板");
		return false;
	}

	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

void KQtClipboardAdapter::handleClipboardChanged()
{
	const QMimeData *pMimeData = m_pClipboard->mimeData(QClipboard::Clipboard);
	if (pMimeData != nullptr && pMimeData->hasText())
		emit textChanged(pMimeData->text());
}
