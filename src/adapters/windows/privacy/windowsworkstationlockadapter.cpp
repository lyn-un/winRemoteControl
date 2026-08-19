#include "adapters/windows/privacy/windowsworkstationlockadapter.h"

#include <Windows.h>

bool KWindowsWorkstationLockAdapter::isSupported() const
{
	return true;
}

KPrivacyOperationResult KWindowsWorkstationLockAdapter::lock()
{
	if (LockWorkStation())
		return KPrivacyOperationResult::success();
	return KPrivacyOperationResult::failure(QStringLiteral("apply_failed"),
		QStringLiteral("LockWorkStation failed: %1").arg(GetLastError()));
}
