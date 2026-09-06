#include "render/remotemouseclicktracker.h"

void KRemoteMouseClickTracker::clear()
{
	m_lastCompletedClickPoint = QPoint();
	m_activeDoubleClickPoint = QPoint();
	m_nLastCompletedClickButton = 0;
	m_nActiveDoubleClickButton = 0;
	m_bHasLastCompletedClick = false;
}

void KRemoteMouseClickTracker::recordCompletedClick(int nButton,
	const QPoint &remotePoint)
{
	if (nButton <= 0)
		return;
	m_nLastCompletedClickButton = nButton;
	m_lastCompletedClickPoint = remotePoint;
	m_bHasLastCompletedClick = true;
}

QPoint KRemoteMouseClickTracker::beginDoubleClick(int nButton,
	const QPoint &currentRemotePoint)
{
	m_nActiveDoubleClickButton = nButton;
	m_activeDoubleClickPoint = m_bHasLastCompletedClick
		&& m_nLastCompletedClickButton == nButton
		? m_lastCompletedClickPoint
		: currentRemotePoint;
	return m_activeDoubleClickPoint;
}

bool KRemoteMouseClickTracker::takeDoubleClickReleasePoint(int nButton,
	QPoint *pRemotePoint)
{
	if (pRemotePoint == nullptr
		|| nButton <= 0
		|| nButton != m_nActiveDoubleClickButton)
	{
		return false;
	}

	*pRemotePoint = m_activeDoubleClickPoint;
	m_activeDoubleClickPoint = QPoint();
	m_nActiveDoubleClickButton = 0;
	return true;
}
