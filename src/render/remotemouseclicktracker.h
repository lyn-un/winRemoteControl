#ifndef _WINREMOTECONTROL_REMOTEMOUSECLICKTRACKER_H_
#define _WINREMOTECONTROL_REMOTEMOUSECLICKTRACKER_H_

#include <QtCore/QPoint>

class KRemoteMouseClickTracker
{
public:
	void clear();
	void recordCompletedClick(int nButton, const QPoint &remotePoint);
	QPoint beginDoubleClick(int nButton, const QPoint &currentRemotePoint);
	bool takeDoubleClickReleasePoint(int nButton, QPoint *pRemotePoint);

private:
	QPoint m_lastCompletedClickPoint;
	QPoint m_activeDoubleClickPoint;
	int m_nLastCompletedClickButton = 0;
	int m_nActiveDoubleClickButton = 0;
	bool m_bHasLastCompletedClick = false;
};

#endif // _WINREMOTECONTROL_REMOTEMOUSECLICKTRACKER_H_
