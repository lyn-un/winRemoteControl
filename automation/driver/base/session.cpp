#include "automation/driver/base/session.h"

#include <QtCore/QUuid>

bool KDriverSession::isValid() const
{
	return !bQuit && !QUuid(strSessionId).isNull()
		&& nCreatedAtMs > 0 && nLastActivityAtMs >= nCreatedAtMs;
}

void KDriverSession::touch(qint64 nNowMs)
{
	if (!bQuit && nNowMs >= nLastActivityAtMs)
		nLastActivityAtMs = nNowMs;
}
