#include "adapters/windows/terminal/powershelllaunchpolicy.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace
{
	void AppendCandidate(QVector<KPowerShellCandidate> *pCandidates,
		const QString &strPath,
		KPowerShellEdition edition,
		bool bFallback)
	{
		if (pCandidates == nullptr || strPath.isEmpty())
			return;
		const QFileInfo fileInfo(strPath);
		if (!fileInfo.isFile())
			return;
		const QString strAbsolutePath = QDir::cleanPath(fileInfo.absoluteFilePath());
		for (const KPowerShellCandidate &candidate : *pCandidates)
		{
			if (candidate.strExecutablePath.compare(strAbsolutePath, Qt::CaseInsensitive) == 0)
				return;
		}
		pCandidates->append({ strAbsolutePath, edition, bFallback });
	}
}

bool KPowerShellStartResult::succeeded() const
{
	return nSelectedIndex >= 0;
}

QVector<KPowerShellCandidate> ResolvePowerShellCandidates(
	const QString &strStandardPowerShell7Path,
	const QString &strPathPowerShell7Path,
	const QString &strWindowsPowerShellPath)
{
	QVector<KPowerShellCandidate> candidates;
	AppendCandidate(&candidates, strStandardPowerShell7Path, PowerShell7Edition, false);
	AppendCandidate(&candidates, strPathPowerShell7Path, PowerShell7Edition, false);
	AppendCandidate(&candidates, strWindowsPowerShellPath, WindowsPowerShell51Edition, true);
	return candidates;
}

KPowerShellStartResult StartPowerShellCandidate(
	const QVector<KPowerShellCandidate> &candidates,
	const KPowerShellStartFunction &startFunction)
{
	KPowerShellStartResult result;
	if (!startFunction)
		return result;
	for (int nIndex = 0; nIndex < candidates.size(); ++nIndex)
	{
		result.nLastError = startFunction(candidates.at(nIndex));
		if (result.nLastError == 0)
		{
			result.nSelectedIndex = nIndex;
			return result;
		}
	}
	return result;
}

QString PowerShellEditionName(KPowerShellEdition edition)
{
	return edition == PowerShell7Edition
		? QStringLiteral("powershell7")
		: QStringLiteral("windows_powershell_5_1");
}
