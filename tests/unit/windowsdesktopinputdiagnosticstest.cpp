#include "adapters/windows/input/windowsdesktopinputdiagnostics.h"

#include <Windows.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

namespace
{
int g_nFailureCount = 0;

void Check(bool bCondition, const QString &strDescription)
{
	if (bCondition)
		return;
	qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
	++g_nFailureCount;
}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	Check(KWindowsDesktopInputDiagnostics::classifyWindow(
		QStringLiteral("SysListView32"), QStringLiteral("explorer.exe"))
		== DesktopListViewInputWindowKind,
		QStringLiteral("desktop list view class is classified"));
	Check(KWindowsDesktopInputDiagnostics::classifyWindow(
		QStringLiteral("WorkerW"), QStringLiteral("explorer.exe"))
		== WorkerInputWindowKind,
		QStringLiteral("WorkerW is classified"));
	Check(KWindowsDesktopInputDiagnostics::classifyWindow(
		QStringLiteral("CustomWindow"), QStringLiteral("N0vaDesktop.exe"))
		== N0vaDesktopInputWindowKind,
		QStringLiteral("N0va process takes precedence over its custom class"));
	Check(KWindowsDesktopInputDiagnostics::isDesktopListViewHierarchy(
		QStringLiteral("explorer.exe"),
		{QStringLiteral("SHELLDLL_DefView"), QStringLiteral("WorkerW")}),
		QStringLiteral("Explorer desktop list view hierarchy is accepted"));
	Check(!KWindowsDesktopInputDiagnostics::isDesktopListViewHierarchy(
		QStringLiteral("explorer.exe"),
		{QStringLiteral("SHELLDLL_DefView"), QStringLiteral("CabinetWClass")}),
		QStringLiteral("ordinary Explorer folder is not treated as the desktop"));
	Check(!KWindowsDesktopInputDiagnostics::isDesktopListViewHierarchy(
		QStringLiteral("other.exe"),
		{QStringLiteral("SHELLDLL_DefView"), QStringLiteral("WorkerW")}),
		QStringLiteral("non-Explorer hierarchy is rejected"));

	const QString strProcessToken =
		KWindowsDesktopInputDiagnostics::processFileNameToken(
			QStringLiteral("C:/Users/private-name/Secret Folder/N0va Desktop.exe"));
	Check(strProcessToken == QStringLiteral("N0va_Desktop.exe")
		&& !strProcessToken.contains(QStringLiteral("private-name"))
		&& !strProcessToken.contains(QStringLiteral("Secret")),
		QStringLiteral("process logging strips directories and whitespace"));
	Check(KWindowsDesktopInputDiagnostics::integrityLevelName(
		SECURITY_MANDATORY_MEDIUM_RID) == QStringLiteral("medium"),
		QStringLiteral("medium process integrity is classified"));
	Check(KWindowsDesktopInputDiagnostics::integrityLevelName(
		SECURITY_MANDATORY_HIGH_RID) == QStringLiteral("high"),
		QStringLiteral("high process integrity is classified"));
	const QString strInjectedFlags =
		KWindowsDesktopInputDiagnostics::mouseHookFlagsToken(
			LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED);
	Check(strInjectedFlags.contains(QStringLiteral("injected=1"))
		&& strInjectedFlags.contains(QStringLiteral("lowerIlInjected=1")),
		QStringLiteral("low-level mouse injection flags are decoded"));
	const QString strPhysicalFlags =
		KWindowsDesktopInputDiagnostics::mouseHookFlagsToken(0);
	Check(strPhysicalFlags.contains(QStringLiteral("injected=0"))
		&& strPhysicalFlags.contains(QStringLiteral("lowerIlInjected=0")),
		QStringLiteral("physical mouse flags remain clear"));

	KWindowsDesktopInputDiagnostics disabledDiagnostics;
	Check(disabledDiagnostics.beginMouseButton(10, 20, 1, true, false) == 0,
		QStringLiteral("disabled diagnostics performs no event work"));
	return g_nFailureCount == 0 ? 0 : 1;
}
