#include "session/peerlifecyclecontroller.h"

#include "core/transport/remotepeertransport.h"

#include <QtCore/QTimer>

namespace
{
	constexpr int kRollbackTimeoutMs = 3000;
}

KPeerLifecycleController::KPeerLifecycleController(
	KRemotePeerTransport *pTransport, QObject *pParent)
	: QObject(pParent)
	, m_pTransport(pTransport)
	, m_pRollbackTimer(new QTimer(this))
{
	Q_ASSERT(m_pTransport != nullptr);
	m_pRollbackTimer->setSingleShot(true);
	m_pRollbackTimer->setInterval(kRollbackTimeoutMs);
	connect(m_pRollbackTimer, &QTimer::timeout, this, [this]()
		{
			if (!m_bRollbackPending || m_bRollbackTimedOut)
				return;
			m_bRollbackTimedOut = true;
			emit rollbackTimeout(m_nGeneration, kRollbackTimeoutMs);
		});
	connect(m_pTransport, &KRemotePeerTransport::shutdownFinished,
		this, &KPeerLifecycleController::handleShutdownFinished);
}

KPeerInitializationResult KPeerLifecycleController::initialize(KSessionRole role)
{
	if (m_bRollbackPending)
	{
		KPeerInitializationResult result;
		result.status = RejectedPeerInitializationStatus;
		result.strTechnicalMessage = QStringLiteral("peer rollback is pending");
		return result;
	}
	++m_nGeneration;
	const KPeerInitializationResult result = m_pTransport->initialize(role, m_nGeneration);
	if (result.status == RollbackPendingPeerInitializationStatus)
	{
		m_bRollbackPending = true;
		m_bRollbackTimedOut = false;
		m_pRollbackTimer->start();
		emit rollbackStarted(m_nGeneration, kRollbackTimeoutMs);
	}
	return result;
}

void KPeerLifecycleController::requestShutdown(quint64 nCompletionGeneration)
{
	m_nShutdownCompletionGeneration = nCompletionGeneration;
	m_pTransport->requestShutdown(m_nGeneration);
}

quint64 KPeerLifecycleController::generation() const
{
	return m_nGeneration;
}

bool KPeerLifecycleController::rollbackPending() const
{
	return m_bRollbackPending;
}

bool KPeerLifecycleController::rollbackTimedOut() const
{
	return m_bRollbackTimedOut;
}

void KPeerLifecycleController::handleShutdownFinished(quint64 nGeneration)
{
	if (nGeneration != m_nGeneration)
		return;
	if (m_bRollbackPending)
	{
		m_pRollbackTimer->stop();
		const bool bFinishedAfterTimeout = m_bRollbackTimedOut;
		m_bRollbackPending = false;
		m_bRollbackTimedOut = false;
		emit rollbackFinished(nGeneration, bFinishedAfterTimeout);
	}
	const quint64 nCompletionGeneration = m_nShutdownCompletionGeneration != 0
		? m_nShutdownCompletionGeneration : nGeneration;
	m_nShutdownCompletionGeneration = 0;
	emit peerShutdownFinished(nCompletionGeneration);
}
