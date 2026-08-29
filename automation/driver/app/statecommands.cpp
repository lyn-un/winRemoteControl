#include "automation/driver/app/statecommands.h"

QByteArray DriverStateSnapshotKind()
{
	return QByteArrayLiteral("state");
}

QByteArray DriverEventsSnapshotKind()
{
	return QByteArrayLiteral("events");
}
