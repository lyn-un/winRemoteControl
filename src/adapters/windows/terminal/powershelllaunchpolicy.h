#ifndef _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_POWERSHELLLAUNCHPOLICY_H_
#define _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_POWERSHELLLAUNCHPOLICY_H_

#include <QtCore/QString>
#include <QtCore/QVector>

#include <functional>

enum KPowerShellEdition
{
	PowerShell7Edition,
	WindowsPowerShell51Edition
};

struct KPowerShellCandidate
{
	QString strExecutablePath;
	KPowerShellEdition edition = WindowsPowerShell51Edition;
	bool bFallback = false;
};

struct KPowerShellStartResult
{
	int nSelectedIndex = -1;
	quint32 nLastError = 2;

	bool succeeded() const;
};

using KPowerShellStartFunction =
	std::function<quint32(const KPowerShellCandidate &candidate)>;

QVector<KPowerShellCandidate> ResolvePowerShellCandidates(
	const QString &strStandardPowerShell7Path,
	const QString &strPathPowerShell7Path,
	const QString &strWindowsPowerShellPath);
KPowerShellStartResult StartPowerShellCandidate(
	const QVector<KPowerShellCandidate> &candidates,
	const KPowerShellStartFunction &startFunction);
QString PowerShellEditionName(KPowerShellEdition edition);

#endif // _WINREMOTECONTROL_ADAPTERS_WINDOWS_TERMINAL_POWERSHELLLAUNCHPOLICY_H_
