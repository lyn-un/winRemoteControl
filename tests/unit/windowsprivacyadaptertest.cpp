#include "adapters/windows/privacy/windowsprivacyoverlayadapter.h"

#include <QtCore/QCoreApplication>

#include <Windows.h>

#include <iostream>

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	int nFailures = 0;
	const auto check = [&nFailures](bool bCondition, const char *pDescription)
	{
		if (bCondition)
			return;
		std::cerr << "FAILED: " << pDescription << '\n';
		++nFailures;
	};
	check(KWindowsPrivacyOverlayAdapter::isPhysicalRestoreShortcut(
		'P', 0, true, true, true),
		"physical Ctrl+Alt+Shift+P is accepted");
	check(!KWindowsPrivacyOverlayAdapter::isPhysicalRestoreShortcut(
		'P', LLKHF_INJECTED, true, true, true),
		"injected restore shortcut is rejected");
	check(!KWindowsPrivacyOverlayAdapter::isPhysicalRestoreShortcut(
		'P', LLKHF_LOWER_IL_INJECTED, true, true, true),
		"lower integrity injected shortcut is rejected");
	check(!KWindowsPrivacyOverlayAdapter::isPhysicalRestoreShortcut(
		'P', 0, true, false, true),
		"incomplete modifiers are rejected");
	return nFailures == 0 ? 0 : 1;
}
