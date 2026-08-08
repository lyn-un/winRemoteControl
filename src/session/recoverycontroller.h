#ifndef _WINREMOTECONTROL_SESSION_RECOVERYCONTROLLER_H_
#define _WINREMOTECONTROL_SESSION_RECOVERYCONTROLLER_H_

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>

class KRecoveryController : public QObject
{
	Q_OBJECT

public:
	explicit KRecoveryController(QObject *pParent = nullptr);

	KRecoveryController(const KRecoveryController &) = delete;
	KRecoveryController &operator=(const KRecoveryController &) = delete;

	void begin(quint64 nGeneration, int nTimeoutMs);
	qint64 complete(quint64 nGeneration);
	qint64 clear();
	bool isActive(quint64 nGeneration) const;
	qint64 elapsedMs() const;
	quint64 generation() const;

signals:
	void timedOut(quint64 nGeneration);

private:
	class QTimer *m_pTimer = nullptr;
	QElapsedTimer m_elapsedTimer;
	quint64 m_nGeneration = 0;
};

#endif // _WINREMOTECONTROL_SESSION_RECOVERYCONTROLLER_H_
