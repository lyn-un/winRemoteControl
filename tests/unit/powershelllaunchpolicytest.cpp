#include "adapters/windows/terminal/powershelllaunchpolicy.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <iostream>

namespace
{
	bool Require(bool bCondition, const char *pMessage)
	{
		if (bCondition)
			return true;
		std::cerr << pMessage << '\n';
		return false;
	}
}

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	const QDir sourceDirectory(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath());
	const QString strStandardPath = sourceDirectory.filePath("powershelllaunchpolicytest.cpp");
	const QString strPathPath = sourceDirectory.filePath("windowspseudoconsoletest.cpp");
	const QString strFallbackPath = sourceDirectory.filePath("terminalsessionservicetest.cpp");

	QVector<KPowerShellCandidate> candidates = ResolvePowerShellCandidates(
		strStandardPath, strPathPath, strFallbackPath);
	if (!Require(candidates.size() == 3, "Candidate count is incorrect")
		|| !Require(candidates.at(0).strExecutablePath == strStandardPath,
			"Standard PowerShell 7 is not preferred")
		|| !Require(candidates.at(1).strExecutablePath == strPathPath,
			"PATH PowerShell 7 is not second")
		|| !Require(candidates.at(2).edition == WindowsPowerShell51Edition,
			"Windows PowerShell fallback is missing"))
	{
		return 1;
	}

	candidates = ResolvePowerShellCandidates(strStandardPath, strStandardPath,
		sourceDirectory.filePath("missing.exe"));
	if (!Require(candidates.size() == 1, "Duplicate or missing candidates were not filtered"))
		return 1;

	candidates = ResolvePowerShellCandidates(strStandardPath, QString(), strFallbackPath);
	int nAttemptCount = 0;
	const KPowerShellStartResult fallbackResult = StartPowerShellCandidate(candidates,
		[&nAttemptCount](const KPowerShellCandidate &candidate) -> quint32
		{
			++nAttemptCount;
			return candidate.edition == PowerShell7Edition ? 193 : 0;
		});
	if (!Require(fallbackResult.succeeded(), "Fallback did not succeed")
		|| !Require(fallbackResult.nSelectedIndex == 1, "Fallback selected the wrong candidate")
		|| !Require(nAttemptCount == 2, "Fallback attempt count is incorrect"))
	{
		return 1;
	}

	const KPowerShellStartResult failureResult = StartPowerShellCandidate(candidates,
		[](const KPowerShellCandidate &) -> quint32 { return 5; });
	if (!Require(!failureResult.succeeded(), "All-failed launch unexpectedly succeeded")
		|| !Require(failureResult.nLastError == 5, "Final launch error was not preserved"))
	{
		return 1;
	}
	return 0;
}
