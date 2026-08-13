#ifndef _WINREMOTECONTROL_SESSION_PEERLIFECYCLECONTROLLER_H_
#define _WINREMOTECONTROL_SESSION_PEERLIFECYCLECONTROLLER_H_

#include "core/session/sessionstatemachine.h"
#include "core/transport/peerinitializationresult.h"

#include <QtCore/QObject>

class KRemotePeerTransport;

class KPeerLifecycleController final : public QObject
{
	Q_OBJECT

public:
	explicit KPeerLifecycleController(KRemotePeerTransport *pTransport,
		QObject *pParent = nullptr);
	KPeerInitializationResult initialize(KSessionRole role);
	void requestShutdown(quint64 nCompletionGeneration);
	quint64 generation() const;
	bool rollbackPending() const;
	bool rollbackTimedOut() const;

signals:
	void rollbackStarted(quint64 nGeneration, int nTimeoutMs);
	void rollbackTimeout(quint64 nGeneration, int nTimeoutMs);
	void rollbackFinished(quint64 nGeneration, bool bFinishedAfterTimeout);
	void peerShutdownFinished(quint64 nGeneration);

private:
	void handleShutdownFinished(quint64 nGeneration);

	KRemotePeerTransport *m_pTransport = nullptr;
	class QTimer *m_pRollbackTimer = nullptr;
	quint64 m_nGeneration = 0;
	quint64 m_nShutdownCompletionGeneration = 0;
	bool m_bRollbackPending = false;
	bool m_bRollbackTimedOut = false;
};

#endif // _WINREMOTECONTROL_SESSION_PEERLIFECYCLECONTROLLER_H_
