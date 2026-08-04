#include "adapters/clipboard/qtclipboardadapter.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QMimeData>
#include <QtCore/QTimer>

#include <Windows.h>

#include "common/sessiontracelogger.h"

namespace
{
	constexpr int kClipboardReadDelayMs = 50;
	constexpr int kMaximumClipboardReadRetries = 3;

	quint32 ClipboardSequence()
	{
		return static_cast<quint32>(::GetClipboardSequenceNumber());
	}

	bool IsClipboardBusy()
	{
		return ::GetOpenClipboardWindow() != nullptr;
	}
}

KQtClipboardAdapter::KQtClipboardAdapter(QObject *pParent)
	: KClipboardAdapter(pParent)
	, m_pClipboard(QGuiApplication::clipboard())
	, m_pReadTimer(new QTimer(this))
{
	m_pReadTimer->setSingleShot(true);
	connect(m_pReadTimer, &QTimer::timeout,
		this, &KQtClipboardAdapter::processPendingClipboardChange);
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
	if (IsClipboardBusy())
	{
		logIgnoredChange(QStringLiteral("busy"), ClipboardSequence());
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("系统剪贴板正被占用");
		return false;
	}

	m_changeFilter.prepareSelfWrite(strText);
	m_pClipboard->setText(strText, QClipboard::Clipboard);
	m_changeFilter.confirmSelfWrite(ClipboardSequence());
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

void KQtClipboardAdapter::handleClipboardChanged()
{
	m_nReadRetryCount = 0;
	m_pReadTimer->start(kClipboardReadDelayMs);
}

void KQtClipboardAdapter::processPendingClipboardChange()
{
	if (IsClipboardBusy())
	{
		if (m_nReadRetryCount < kMaximumClipboardReadRetries)
		{
			++m_nReadRetryCount;
			m_pReadTimer->start(kClipboardReadDelayMs);
			return;
		}
		logIgnoredChange(QStringLiteral("busy"), ClipboardSequence());
		return;
	}

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
