#include "automation/wrcdriverhostapi.h"

#include <QtCore/QtGlobal>

#include <cstring>

extern "C" __declspec(dllexport) std::uint32_t wrcDriverAbiVersion()
{
	return KWrcDriverAbiVersion1 + 1;
}

extern "C" __declspec(dllexport) bool wrcDriverBuildInfo(
	KWrcDriverBuildInfoV1 *pBuildInfo)
{
	if (pBuildInfo == nullptr)
		return false;
	*pBuildInfo = KWrcDriverBuildInfoV1();
	pBuildInfo->nQtMajorVersion = QT_VERSION_MAJOR;
	std::memset(pBuildInfo->szBuildId, 0, sizeof(pBuildInfo->szBuildId));
	std::memcpy(pBuildInfo->szBuildId, KWrcAutomationBuildId,
		sizeof(KWrcAutomationBuildId));
	return true;
}

extern "C" __declspec(dllexport) bool wrcDriverStartup(
	const KWrcDriverHostApiV1 *)
{
	return true;
}

extern "C" __declspec(dllexport) void wrcDriverShutdown()
{
}
