#include "transport/webrtc/webrtccallbackgate.h"

#include <QtCore/QMetaObject>
#include <QtCore/QObject>

#include <memory>
#include <utility>

void KWebRtcCallbackGate::open(QObject *pTarget, quint64 nGeneration)
{
	std::lock_guard<std::mutex> guard(m_mutex);
	m_pTarget = pTarget;
	m_nGeneration = nGeneration;
}

void KWebRtcCallbackGate::close()
{
	std::lock_guard<std::mutex> guard(m_mutex);
	m_pTarget = nullptr;
	m_nGeneration = 0;
}

bool KWebRtcCallbackGate::post(quint64 nGeneration, Callback callback)
{
	std::lock_guard<std::mutex> guard(m_mutex);
	if (m_pTarget == nullptr || nGeneration != m_nGeneration)
		return false;
	QObject *pTarget = m_pTarget;
	std::shared_ptr<KWebRtcCallbackGate> spGate = shared_from_this();
	return QMetaObject::invokeMethod(m_pTarget,
		[spGate, pTarget, nGeneration, callback = std::move(callback)]()
		{
			if (spGate->isCurrent(pTarget, nGeneration))
				callback(pTarget);
		},
		Qt::QueuedConnection);
}

bool KWebRtcCallbackGate::isCurrent(QObject *pTarget, quint64 nGeneration) const
{
	std::lock_guard<std::mutex> guard(m_mutex);
	return m_pTarget == pTarget && m_nGeneration == nGeneration;
}
