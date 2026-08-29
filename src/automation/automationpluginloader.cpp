#include "automation/automationpluginloader.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QtGlobal>

namespace
{
	std::uint32_t RuntimeFlavor()
	{
#ifdef QT_DEBUG
		return KWrcDriverRuntimeDebug;
#else
		return KWrcDriverRuntimeRelease;
#endif
	}
}

KAutomationPluginLoader::KAutomationPluginLoader()
{
}

KAutomationPluginLoader::~KAutomationPluginLoader()
{
	shutdown();
}

bool KAutomationPluginLoader::load(const QString &strApplicationDirectory,
	const KWrcDriverHostApiV1 *pHostApi,
	QString *pErrorMessage)
{
	if (m_bStarted)
		return true;
	if (pHostApi == nullptr || pHostApi->nAbiVersion != KWrcDriverAbiVersion1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Invalid automation Host API");
		return false;
	}

	const QString strLibraryPath = QDir(strApplicationDirectory)
		.filePath(QStringLiteral("automation/wrcdriver.dll"));
	if (!QFileInfo::exists(strLibraryPath))
	{
		if (pErrorMessage != nullptr)
			pErrorMessage->clear();
		return true;
	}

	m_library.setFileName(strLibraryPath);
	if (!m_library.load())
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = m_library.errorString();
		return false;
	}

	const auto pAbiVersion = reinterpret_cast<KWrcDriverAbiVersionFunction>(
		m_library.resolve("wrcDriverAbiVersion"));
	const auto pBuildInfo = reinterpret_cast<KWrcDriverBuildInfoFunction>(
		m_library.resolve("wrcDriverBuildInfo"));
	const auto pStartup = reinterpret_cast<KWrcDriverStartupFunction>(
		m_library.resolve("wrcDriverStartup"));
	m_pShutdown = reinterpret_cast<KWrcDriverShutdownFunction>(
		m_library.resolve("wrcDriverShutdown"));
	if (pAbiVersion == nullptr || pBuildInfo == nullptr
		|| pStartup == nullptr || m_pShutdown == nullptr)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Automation driver exports are incomplete");
		m_library.unload();
		return false;
	}
	if (pAbiVersion() != KWrcDriverAbiVersion1)
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Automation driver ABI version mismatch");
		m_library.unload();
		return false;
	}

	KWrcDriverBuildInfoV1 buildInfo;
	if (!pBuildInfo(&buildInfo) || !validateBuildInfo(buildInfo, pErrorMessage))
	{
		m_library.unload();
		return false;
	}
	if (!pStartup(pHostApi))
	{
		if (pErrorMessage != nullptr)
			*pErrorMessage = QStringLiteral("Automation driver startup failed");
		m_library.unload();
		return false;
	}
	m_bStarted = true;
	m_library.setLoadHints(QLibrary::PreventUnloadHint);
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	return true;
}

void KAutomationPluginLoader::shutdown()
{
	if (!m_bStarted)
		return;
	m_bStarted = false;
	if (m_pShutdown != nullptr)
		m_pShutdown();
}

bool KAutomationPluginLoader::isLoaded() const
{
	return m_bStarted;
}

bool KAutomationPluginLoader::validateBuildInfo(const KWrcDriverBuildInfoV1 &buildInfo,
	QString *pErrorMessage) const
{
	const QByteArray rawBuildId(buildInfo.szBuildId,
		static_cast<qsizetype>(sizeof(buildInfo.szBuildId)));
	const qsizetype nTerminator = rawBuildId.indexOf('\0');
	const QByteArray buildId = nTerminator >= 0
		? rawBuildId.left(nTerminator) : rawBuildId;
	const bool bValid = buildInfo.nStructSize >= sizeof(KWrcDriverBuildInfoV1)
		&& buildInfo.nAbiVersion == KWrcDriverAbiVersion1
		&& buildInfo.nArchitecture == KWrcDriverArchitectureX64
		&& buildInfo.nRuntimeFlavor == RuntimeFlavor()
		&& buildInfo.nQtMajorVersion == QT_VERSION_MAJOR
		&& buildId == QByteArray(KWrcAutomationBuildId);
	if (!bValid && pErrorMessage != nullptr)
		*pErrorMessage = QStringLiteral("Automation driver build metadata mismatch");
	return bValid;
}
