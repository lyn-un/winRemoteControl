#include "privacy/postsessionactionservice.h"

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
	emit statusChanged(m_status);
}

void KPostSessionActionService::markStreaming(quint64 nGeneration)
{
	if (nGeneration == m_status.nGeneration && !m_bConsumed)
		m_bEnteredStreaming = true;
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
		emit statusChanged(m_status);
		return KPrivacyOperationResult::success();
	}
	const KPrivacyOperationResult result = m_spLockAdapter->lock();
	if (!result.bSucceeded)
		m_status.strErrorCode = result.strErrorCode;
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
