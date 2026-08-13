#include "session/capabilitysessionflow.h"

#include <QtCore/QTimer>

KCapabilitySessionFlow::KCapabilitySessionFlow(QObject *pParent)
	: QObject(pParent)
	, m_pTimer(new QTimer(this))
{
	m_pTimer->setSingleShot(true);
	connect(m_pTimer, &QTimer::timeout, this, &KCapabilitySessionFlow::timedOut);
}

void KCapabilitySessionFlow::begin(
	const KSessionCapabilities &localCapabilities,
	int nTimeoutMs)
{
	reset();
	m_localCapabilities = localCapabilities;
	m_pTimer->start(qMax(1, nTimeoutMs));
}

KCapabilityNegotiationResult KCapabilitySessionFlow::receive(
	const KSessionCapabilities &remoteCapabilities)
{
	if (m_bComplete)
	{
		KCapabilityNegotiationResult result;
		result.strTechnicalMessage = QStringLiteral("Capabilities already negotiated");
		return result;
	}
	KCapabilityNegotiationResult result = KCapabilityNegotiator::negotiate(
		m_localCapabilities, remoteCapabilities);
	if (result.succeeded())
	{
		m_pTimer->stop();
		m_negotiatedCapabilities = result.capabilities;
		m_bComplete = true;
	}
	return result;
}

void KCapabilitySessionFlow::reset()
{
	m_pTimer->stop();
	m_localCapabilities = KSessionCapabilities();
	m_negotiatedCapabilities = KNegotiatedCapabilities();
	m_bComplete = false;
}

bool KCapabilitySessionFlow::isComplete() const
{
	return m_bComplete;
}

const KSessionCapabilities &KCapabilitySessionFlow::localCapabilities() const
{
	return m_localCapabilities;
}

const KNegotiatedCapabilities &KCapabilitySessionFlow::negotiatedCapabilities() const
{
	return m_negotiatedCapabilities;
}
