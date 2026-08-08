#ifndef _WINREMOTECONTROL_SESSION_SHUTDOWNCOORDINATOR_H_
#define _WINREMOTECONTROL_SESSION_SHUTDOWNCOORDINATOR_H_

#include <QtCore/QObject>

class KShutdownCoordinator : public QObject
{
	Q_OBJECT

public:
	explicit KShutdownCoordinator(QObject *pParent = nullptr);

	KShutdownCoordinator(const KShutdownCoordinator &) = delete;
	KShutdownCoordinator &operator=(const KShutdownCoordinator &) = delete;

	void begin(quint64 nGeneration,
		bool bCapturePending,
		bool bPeerPending,
		int nWatchdogMs);
	void completeCapture(quint64 nGeneration);
	void completePeer(quint64 nGeneration);
	void clear();
	bool isActive() const;
	bool isCapturePending() const;
	bool isPeerPending() const;
	quint64 generation() const;

signals:
	void finished(quint64 nGeneration);
	void watchdogExpired(quint64 nGeneration,
		bool bCapturePending,
		bool bPeerPending);

private:
	void tryFinish();

	class QTimer *m_pWatchdogTimer = nullptr;
	quint64 m_nGeneration = 0;
	bool m_bActive = false;
	bool m_bCapturePending = false;
	bool m_bPeerPending = false;
};

#endif // _WINREMOTECONTROL_SESSION_SHUTDOWNCOORDINATOR_H_
