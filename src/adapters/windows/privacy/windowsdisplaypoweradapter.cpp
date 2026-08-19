#include "adapters/windows/privacy/windowsdisplaypoweradapter.h"

#include <Windows.h>

bool KWindowsDisplayPowerAdapter::isSupported() const
{
	return true;
}

KPrivacyOperationResult KWindowsDisplayPowerAdapter::turnOff()
{
	if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) == 0)
	{
		return KPrivacyOperationResult::failure(QStringLiteral("display_power_failed"),
			QStringLiteral("SetThreadExecutionState failed: %1").arg(GetLastError()));
	}
	if (!PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2))
	{
		SetThreadExecutionState(ES_CONTINUOUS);
		return KPrivacyOperationResult::failure(QStringLiteral("display_power_failed"),
			QStringLiteral("Display power-off request failed: %1").arg(GetLastError()));
	}
	return KPrivacyOperationResult::success();
}

KPrivacyOperationResult KWindowsDisplayPowerAdapter::restore()
{
	const BOOL bPosted = PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND,
		SC_MONITORPOWER, static_cast<LPARAM>(-1));
	const EXECUTION_STATE state = SetThreadExecutionState(ES_CONTINUOUS);
	if (!bPosted || state == 0)
	{
		return KPrivacyOperationResult::failure(QStringLiteral("restore_failed"),
			QStringLiteral("Display power restore request failed: %1").arg(GetLastError()));
	}
	return KPrivacyOperationResult::success();
}
