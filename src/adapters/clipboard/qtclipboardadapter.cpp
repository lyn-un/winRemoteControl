#include "adapters/clipboard/qtclipboardadapter.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QMimeData>

#include <Windows.h>

#include "common/sessiontracelogger.h"

namespace
{
	quint32 ClipboardSequence()
	{
		return static_cast<quint32>(::GetClipboardSequenceNumber());
	}
}

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
	m_changeFilter.prepareSelfWrite(strText);
	m_pClipboard->setText(strText, QClipboard::Clipboard);
	m_changeFilter.confirmSelfWrite(ClipboardSequence());
	if (text() != strText)
	{
		m_changeFilter.cancelSelfWrite();
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
	if (pMimeData == nullptr)
		return;

	const quint32 nSequence = ClipboardSequence();
	const bool bHasFileData = KClipboardChangeFilter::hasFileData(*pMimeData);
	const QString strText = pMimeData->hasText() ? pMimeData->text() : QString();
	const KClipboardChangeDecision decision = m_changeFilter.evaluate(
		nSequence, m_pClipboard->ownsClipboard(), strText, bHasFileData);
	if (decision == SelfWriteClipboardChangeDecision)
	{
		logIgnoredChange(QStringLiteral("self_write"), nSequence);
		return;
	}
	if (decision == DuplicateSequenceClipboardChangeDecision)
	{
		logIgnoredChange(QStringLiteral("duplicate_sequence"), nSequence);
		return;
	}
	if (decision == FileClipboardChangeDecision)
	{
		logIgnoredChange(QStringLiteral("file_clipboard"), nSequence);
		return;
	}
	if (pMimeData->hasText())
		emit textChanged(strText);
}

void KQtClipboardAdapter::logIgnoredChange(const QString &strReason, quint32 nSequence) const
{
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("clipboard_drop"),
		strReason,
		-1,
		QStringLiteral("sequence=%1").arg(nSequence));
}
