#include "render/remotemouseclicktracker.h"

#include <QtCore/QCoreApplication>

#include <iostream>

namespace
{
	bool Require(bool bCondition, const char *pMessage)
	{
		if (bCondition)
			return true;
		std::cerr << pMessage << std::endl;
		return false;
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	KRemoteMouseClickTracker tracker;
	constexpr int kLeftButton = 1;
	constexpr int kRightButton = 2;

	tracker.recordCompletedClick(kLeftButton, QPoint(400, 300));
	const QPoint anchoredPoint = tracker.beginDoubleClick(
		kLeftButton, QPoint(407, 305));
	bool bSuccess = Require(anchoredPoint == QPoint(400, 300),
		"double click did not reuse the first click coordinate");

	QPoint releasePoint;
	bSuccess &= Require(tracker.takeDoubleClickReleasePoint(
			kLeftButton, &releasePoint)
			&& releasePoint == QPoint(400, 300),
		"double click release did not reuse the anchored coordinate");
	bSuccess &= Require(!tracker.takeDoubleClickReleasePoint(
			kLeftButton, &releasePoint),
		"completed double click retained an active release coordinate");

	tracker.recordCompletedClick(kLeftButton, QPoint(100, 80));
	bSuccess &= Require(tracker.beginDoubleClick(
			kRightButton, QPoint(220, 160)) == QPoint(220, 160),
		"a different mouse button reused the previous click coordinate");
	bSuccess &= Require(tracker.takeDoubleClickReleasePoint(
			kRightButton, &releasePoint),
		"right-button double click did not retain its release coordinate");

	tracker.recordCompletedClick(kLeftButton, QPoint(50, 60));
	tracker.beginDoubleClick(kLeftButton, QPoint(51, 61));
	tracker.clear();
	bSuccess &= Require(!tracker.takeDoubleClickReleasePoint(
			kLeftButton, &releasePoint),
		"clear did not remove the active double click state");
	bSuccess &= Require(tracker.beginDoubleClick(
			kLeftButton, QPoint(70, 90)) == QPoint(70, 90),
		"clear did not remove the cached completed click");

	return bSuccess ? 0 : 1;
}
