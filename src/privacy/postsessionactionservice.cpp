#include "privacy/postsessionactionservice.h"

#include "common/sessiontracelogger.h"

KPostSessionActionService::KPostSessionActionService(
	std::unique_ptr<IKWorkstationLockAdapter> spLockAdapter,
	QObject *pParent)
	: QObject(pParent)
	, m_spLockAdapter(std::move(spLockAdapter))
{
	Q_ASSERT(m_spLockAdapter != nullptr);
}

void KPostSessionActionService::beginSession(quint64 nGeneration)
{
	m_status = KPostSessionActionStatus();
	m_status.nGeneration = nGeneration;
	m_bEnteredStreaming = false;
	m_bConsumed = false;
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("post_session_action"), QStringLiteral("initialized"), -1,
		QStringLiteral("generation=%1 action=none").arg(nGeneration));
	emit statusChanged(m_status);
}

void KPostSessionActionService::markStreaming(quint64 nGeneration)
{
	if (nGeneration == m_status.nGeneration && !m_bConsumed)
	{
		m_bEnteredStreaming = true;
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("post_session_action"), QStringLiteral("streaming_seen"), -1,
			QStringLiteral("generation=%1").arg(nGeneration));
	}
}

KPrivacyOperationResult KPostSessionActionService::setAction(
	KPostSessionAction action,
	const QString &strRequestId,
	quint64 nGeneration)
{
	if (nGeneration == 0 || nGeneration != m_status.nGeneration || m_bConsumed)
	{
		return KPrivacyOperationResult::failure(QStringLiteral("stale_generation"),
			QStringLiteral("Post-session action belongs to another session"));
	}
	if (action == UnknownPostSessionAction
		|| (action == LockWorkstationPostSessionAction && !m_spLockAdapter->isSupported()))
	{
		return KPrivacyOperationResult::failure(QStringLiteral("unsupported_action"),
			QStringLiteral("Post-session action is unavailable on this host"));
	}
	m_status.action = action;
	m_status.strRequestId = strRequestId;
	m_status.strErrorCode.clear();
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("post_session_action"), QStringLiteral("armed"), -1,
		QStringLiteral("generation=%1 requestId=%2 action=%3")
			.arg(nGeneration).arg(strRequestId,
				action == LockWorkstationPostSessionAction
					? QStringLiteral("lock_workstation") : QStringLiteral("none")));
	emit statusChanged(m_status);
	return KPrivacyOperationResult::success();
}

KPrivacyOperationResult KPostSessionActionService::consumeAfterTeardown(
	quint64 nGeneration)
{
	if (nGeneration != m_status.nGeneration || m_bConsumed)
	{
		return KPrivacyOperationResult::failure(QStringLiteral("stale_generation"),
			QStringLiteral("Post-session action was already consumed or is stale"));
	}
	m_bConsumed = true;
	const KPostSessionAction action = m_status.action;
	m_status.action = NoPostSessionAction;
	m_status.strRequestId.clear();
	if (action != LockWorkstationPostSessionAction || !m_bEnteredStreaming)
	{
		KSessionTraceLogger::write(QStringLiteral("controlled"),
			QStringLiteral("post_session_action"), QStringLiteral("skipped"), -1,
			QStringLiteral("generation=%1 reason=%2")
				.arg(nGeneration).arg(action != LockWorkstationPostSessionAction
					? QStringLiteral("not_armed") : QStringLiteral("never_streamed")));
		emit statusChanged(m_status);
		return KPrivacyOperationResult::success();
	}
	const KPrivacyOperationResult result = m_spLockAdapter->lock();
	if (!result.bSucceeded)
		m_status.strErrorCode = result.strErrorCode;
	KSessionTraceLogger::write(QStringLiteral("controlled"),
		QStringLiteral("post_session_action"),
		result.bSucceeded ? QStringLiteral("executed") : QStringLiteral("failed"), -1,
		QStringLiteral("generation=%1 action=lock_workstation errorCode=%2")
			.arg(nGeneration).arg(result.strErrorCode));
	emit statusChanged(m_status);
	return result;
}

KPostSessionActionStatus KPostSessionActionService::status() const
{
	return m_status;
}

bool KPostSessionActionService::isSupported() const
{
	return m_spLockAdapter->isSupported();
}
