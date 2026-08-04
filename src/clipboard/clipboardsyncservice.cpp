#include "clipboard/clipboardsyncservice.h"

#include "common/sessiontracelogger.h"
#include "core/clipboard/clipboardadapter.h"
#include "session/sessioncontroller.h"

#include <QtCore/QTimer>
#include <QtCore/QUuid>

namespace
{
	constexpr int kMaximumRecentMessageIds = 128;
	constexpr int kMaximumApplyAttempts = 3;
	constexpr int kApplyRetryIntervalMs = 100;

	bool IsTerminalSessionState(KSessionState state)
	{
		return state == IdleSessionState || state == ListeningSessionState;
	}
}

KClipboardSyncService::KClipboardSyncService(
	std::unique_ptr<KClipboardAdapter> spClipboardAdapter,
	KSessionController *pSessionController,
	QObject *pParent)
	: QObject(pParent)
	, m_spClipboardAdapter(std::move(spClipboardAdapter))
	, m_pSessionController(pSessionController)
	, m_pRetryTimer(new QTimer(this))
{
	m_pRetryTimer->setSingleShot(true);
	connect(m_pRetryTimer, &QTimer::timeout,
		this, &KClipboardSyncService::retryPendingApply);
	connect(m_spClipboardAdapter.get(), &KClipboardAdapter::textChanged,
		this, &KClipboardSyncService::handleLocalTextChanged);
	connect(m_pSessionController, &KSessionController::clipboardMessageReceived,
		this, &KClipboardSyncService::handleRemoteMessage);
	connect(m_pSessionController, &KSessionController::clipboardChannelChanged,
		this, &KClipboardSyncService::handleChannelChanged);
	connect(m_pSessionController, &KSessionController::sessionStateChanged,
		this, &KClipboardSyncService::handleSessionStateChanged);
}

KClipboardSyncService::~KClipboardSyncService()
{
	shutdown();
}

void KClipboardSyncService::setEnabled(bool bEnabled)
{
	if (m_bEnabled == bEnabled)
		return;
	m_bEnabled = bEnabled;
	if (!m_bEnabled)
	{
		m_pRetryTimer->stop();
		m_pendingMessage = KClipboardMessage();
		m_nPendingAttempt = 0;
	}
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("clipboard_sync"),
		m_bEnabled ? QStringLiteral("enabled") : QStringLiteral("disabled"));
	if (m_bPeerReady && m_sessionState == StreamingSessionState)
	{
		KClipboardMessage message;
		message.type = SyncStateClipboardMessageType;
		message.strMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		message.bEnabled = m_bEnabled;
		m_pSessionController->sendClipboardMessage(message);
	}
	emitState();
}

void KClipboardSyncService::requestState()
{
	emitState();
}

void KClipboardSyncService::shutdown()
{
	m_pRetryTimer->stop();
	resetSession();
}

void KClipboardSyncService::handleLocalTextChanged(const QString &strText)
{
	if (!isActive())
		return;
	if (strText.isEmpty())
	{
		KSessionTraceLogger::write(QStringLiteral("local"),
			QStringLiteral("clipboard_drop"),
			QStringLiteral("empty_text"),
			0);
		return;
	}

	const int nTextBytes = strText.toUtf8().size();
	if (nTextBytes > KClipboardMessageCodec::kMaximumTextBytes)
	{
		KSessionTraceLogger::write(QStringLiteral("local"),
			QStringLiteral("clipboard_drop"),
			QStringLiteral("too_large"),
			nTextBytes);
		emit syncError(QStringLiteral("剪贴板文本超过 256 KB，未同步"));
		return;
	}

	KClipboardMessage message;
	message.type = TextClipboardMessageType;
	message.strMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	message.strText = strText;
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("clipboard_send"),
		QStringLiteral("text"),
		nTextBytes,
		QStringLiteral("messageId=%1").arg(message.strMessageId));
	m_pSessionController->sendClipboardMessage(message);
}

void KClipboardSyncService::handleRemoteMessage(const KClipboardMessage &message)
{
	if (m_recentMessageIdSet.contains(message.strMessageId))
	{
		KSessionTraceLogger::write(QStringLiteral("local"),
			QStringLiteral("clipboard_drop"),
			QStringLiteral("duplicate"),
			message.strText.toUtf8().size(),
			QStringLiteral("messageId=%1").arg(message.strMessageId));
		return;
	}
	if (message.type == ReadyClipboardMessageType)
	{
		rememberMessageId(message.strMessageId);
		m_bPeerReady = true;
		emitState();
		return;
	}
	if (message.type == SyncStateClipboardMessageType)
	{
		rememberMessageId(message.strMessageId);
		m_bEnabled = message.bEnabled;
		if (!m_bEnabled)
		{
			m_pRetryTimer->stop();
			m_pendingMessage = KClipboardMessage();
			m_nPendingAttempt = 0;
		}
		emitState();
		return;
	}
	if (message.type != TextClipboardMessageType || !isActive())
	{
		KSessionTraceLogger::write(QStringLiteral("local"),
			QStringLiteral("clipboard_drop"),
			QStringLiteral("inactive"),
			message.strText.toUtf8().size(),
			QStringLiteral("messageId=%1").arg(message.strMessageId));
		return;
	}
	if (message.strText.isEmpty())
	{
		rememberMessageId(message.strMessageId);
		KSessionTraceLogger::write(QStringLiteral("local"),
			QStringLiteral("clipboard_drop"),
			QStringLiteral("empty_text"),
			0,
			QStringLiteral("messageId=%1").arg(message.strMessageId));
		return;
	}

	applyRemoteMessage(message, 1);
}

void KClipboardSyncService::handleChannelChanged(bool bOpen)
{
	m_bChannelOpen = bOpen;
	if (!m_bChannelOpen)
	{
		m_bPeerReady = false;
		m_bReadySent = false;
		m_pRetryTimer->stop();
		m_pendingMessage = KClipboardMessage();
		m_nPendingAttempt = 0;
	}
	sendReadyIfNeeded();
	emitState();
}

void KClipboardSyncService::handleSessionStateChanged(KSessionState state)
{
	m_sessionState = state;
	if (state != StreamingSessionState)
	{
		m_pRetryTimer->stop();
		m_pendingMessage = KClipboardMessage();
		m_nPendingAttempt = 0;
	}
	if (state == ConnectedSessionState && !m_bSessionEstablished)
	{
		m_bSessionEstablished = true;
		m_bEnabled = true;
	}
	else if (IsTerminalSessionState(state))
	{
		resetSession();
		m_sessionState = state;
	}
	sendReadyIfNeeded();
	emitState();
}

void KClipboardSyncService::retryPendingApply()
{
	if (m_pendingMessage.strMessageId.isEmpty() || !isActive())
	{
		m_pendingMessage = KClipboardMessage();
		m_nPendingAttempt = 0;
		return;
	}
	// applyRemoteMessage() clears the pending slot before writing. Keep a value
	// copy so the argument cannot alias that slot and become an empty message.
	const KClipboardMessage message = m_pendingMessage;
	const int nAttempt = m_nPendingAttempt + 1;
	applyRemoteMessage(message, nAttempt);
}

void KClipboardSyncService::applyRemoteMessage(const KClipboardMessage &message, int nAttempt)
{
	m_pendingMessage = KClipboardMessage();
	m_nPendingAttempt = 0;
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("clipboard_apply"),
		QStringLiteral("begin"),
		message.strText.toUtf8().size(),
		QStringLiteral("messageId=%1 attempts=%2")
			.arg(message.strMessageId)
			.arg(nAttempt));

	QString strError;
	if (!m_spClipboardAdapter->setText(message.strText, &strError))
	{
		if (nAttempt < kMaximumApplyAttempts && isActive())
		{
			m_pendingMessage = message;
			m_nPendingAttempt = nAttempt;
			m_pRetryTimer->start(kApplyRetryIntervalMs);
			return;
		}

		KSessionTraceLogger::write(QStringLiteral("local"),
			QStringLiteral("clipboard_apply"),
			QStringLiteral("failed"),
			message.strText.toUtf8().size(),
			QStringLiteral("messageId=%1 attempts=%2")
				.arg(message.strMessageId)
				.arg(nAttempt));
		emit syncError(strError.isEmpty()
			? QStringLiteral("无法写入系统剪贴板")
			: strError);
		return;
	}

	rememberMessageId(message.strMessageId);
	KSessionTraceLogger::write(QStringLiteral("local"),
		QStringLiteral("clipboard_apply"),
		QStringLiteral("success"),
		message.strText.toUtf8().size(),
		QStringLiteral("messageId=%1 attempts=%2")
			.arg(message.strMessageId)
			.arg(nAttempt));
}

void KClipboardSyncService::rememberMessageId(const QString &strMessageId)
{
	if (m_recentMessageIdSet.contains(strMessageId))
		return;
	m_recentMessageIdSet.insert(strMessageId);
	m_recentMessageIds.enqueue(strMessageId);
	while (m_recentMessageIds.size() > kMaximumRecentMessageIds)
		m_recentMessageIdSet.remove(m_recentMessageIds.dequeue());
}

void KClipboardSyncService::resetSession()
{
	m_pRetryTimer->stop();
	m_bEnabled = true;
	m_bChannelOpen = false;
	m_bPeerReady = false;
	m_bReadySent = false;
	m_bSessionEstablished = false;
	m_pendingMessage = KClipboardMessage();
	m_nPendingAttempt = 0;
	m_recentMessageIds.clear();
	m_recentMessageIdSet.clear();
}

void KClipboardSyncService::emitState()
{
	const bool bActive = isActive();
	QString strStatus = QStringLiteral("unavailable");
	if (!m_bEnabled)
		strStatus = QStringLiteral("disabled");
	else if (m_sessionState == ReconnectingSessionState)
		strStatus = QStringLiteral("paused");
	else if (bActive)
		strStatus = QStringLiteral("active");
	else if (m_bChannelOpen)
		strStatus = QStringLiteral("waiting");
	emit syncStateChanged(m_bEnabled, m_bChannelOpen && m_bPeerReady, bActive, strStatus);
}

void KClipboardSyncService::sendReadyIfNeeded()
{
	if (m_bReadySent
		|| !m_bChannelOpen
		|| m_sessionState != StreamingSessionState)
	{
		return;
	}

	KClipboardMessage message;
	message.type = ReadyClipboardMessageType;
	message.strMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_bReadySent = true;
	m_pSessionController->sendClipboardMessage(message);
}

bool KClipboardSyncService::isActive() const
{
	return m_bEnabled
		&& m_bPeerReady
		&& m_sessionState == StreamingSessionState;
}
