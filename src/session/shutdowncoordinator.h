#ifndef _WINREMOTECONTROL_SESSION_SHUTDOWNCOORDINATOR_H_
#define _WINREMOTECONTROL_SESSION_SHUTDOWNCOORDINATOR_H_

#include <QtCore/QObject>
#include <QtCore/QElapsedTimer>

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
	bool hasTimedOut() const;
	quint64 generation() const;

signals:
	void finished(quint64 nGeneration, bool bFinishedAfterTimeout);
	void watchdogExpired(quint64 nGeneration,
		bool bCapturePending,
		bool bPeerPending,
		qint64 nElapsedMs);

private:
	void tryFinish();

	class QTimer *m_pWatchdogTimer = nullptr;
	quint64 m_nGeneration = 0;
	bool m_bActive = false;
	bool m_bCapturePending = false;
	bool m_bPeerPending = false;
	bool m_bTimedOut = false;
	QElapsedTimer m_elapsedTimer;
};

#endif // _WINREMOTECONTROL_SESSION_SHUTDOWNCOORDINATOR_H_
