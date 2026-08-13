#ifndef _WINREMOTECONTROL_SESSION_CAPABILITYSESSIONFLOW_H_
#define _WINREMOTECONTROL_SESSION_CAPABILITYSESSIONFLOW_H_

#include "core/session/capabilitynegotiator.h"

#include <QtCore/QObject>

class KCapabilitySessionFlow final : public QObject
{
	Q_OBJECT

public:
	explicit KCapabilitySessionFlow(QObject *pParent = nullptr);
	void begin(const KSessionCapabilities &localCapabilities, int nTimeoutMs);
	KCapabilityNegotiationResult receive(
		const KSessionCapabilities &remoteCapabilities);
	void reset();
	bool isComplete() const;
	const KSessionCapabilities &localCapabilities() const;
	const KNegotiatedCapabilities &negotiatedCapabilities() const;

signals:
	void timedOut();

private:
	class QTimer *m_pTimer = nullptr;
	KSessionCapabilities m_localCapabilities;
	KNegotiatedCapabilities m_negotiatedCapabilities;
	bool m_bComplete = false;
};

#endif // _WINREMOTECONTROL_SESSION_CAPABILITYSESSIONFLOW_H_
