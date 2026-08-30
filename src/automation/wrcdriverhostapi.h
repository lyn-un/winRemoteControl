#ifndef _WINREMOTECONTROL_WRCDRIVERHOSTAPI_H_
#define _WINREMOTECONTROL_WRCDRIVERHOSTAPI_H_

#include <cstdint>

constexpr std::uint32_t KWrcDriverAbiVersion2 = 2;
constexpr std::uint32_t KWrcDriverArchitectureX64 = 1;
constexpr std::uint32_t KWrcDriverRuntimeDebug = 1;
constexpr std::uint32_t KWrcDriverRuntimeRelease = 2;
inline constexpr char KWrcAutomationBuildId[] = "wrc-automation-abi2-20260830";

using KWrcDriverJsonCallback = void (*)(void *pCallbackContext,
	std::uint64_t nRequestId,
	const char *pJsonUtf8,
	std::uint32_t nJsonBytes);

using KWrcDriverCommandStartedCallback = void (*)(void *pCallbackContext,
	std::uint64_t nRequestId);

struct KWrcDriverBuildInfoV2
{
	std::uint32_t nStructSize = sizeof(KWrcDriverBuildInfoV2);
	std::uint32_t nAbiVersion = KWrcDriverAbiVersion2;
	std::uint32_t nArchitecture = KWrcDriverArchitectureX64;
	std::uint32_t nRuntimeFlavor = KWrcDriverRuntimeRelease;
	std::uint32_t nQtMajorVersion = 0;
	char szBuildId[64] = {};
};

struct KWrcDriverHostApiV2
{
	std::uint32_t nStructSize = sizeof(KWrcDriverHostApiV2);
	std::uint32_t nAbiVersion = KWrcDriverAbiVersion2;
	void *pHostContext = nullptr;
	void (*submitCommand)(void *pHostContext,
		std::uint64_t nRequestId,
		const char *pCommandIdUtf8,
		std::uint32_t nCommandIdBytes,
		const char *pArgumentsJsonUtf8,
		std::uint32_t nArgumentsJsonBytes,
		std::uint32_t nTimeoutMs,
		KWrcDriverCommandStartedCallback pStartedCallback,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext) = nullptr;
	void (*requestSnapshot)(void *pHostContext,
		std::uint64_t nRequestId,
		const char *pSnapshotKindUtf8,
		std::uint32_t nSnapshotKindBytes,
		std::uint64_t nSinceSequence,
		KWrcDriverJsonCallback pCallback,
		void *pCallbackContext) = nullptr;
	std::uint32_t (*copyHostValue)(void *pHostContext,
		const char *pKeyUtf8,
		std::uint32_t nKeyBytes,
		char *pDestinationUtf8,
		std::uint32_t nDestinationBytes) = nullptr;
	bool (*isHostReady)(void *pHostContext) = nullptr;
	void (*writeLog)(void *pHostContext,
		std::uint32_t nLevel,
		const char *pMessageUtf8,
		std::uint32_t nMessageBytes) = nullptr;
};

using KWrcDriverAbiVersionFunction = std::uint32_t (*)();
using KWrcDriverBuildInfoFunction = bool (*)(KWrcDriverBuildInfoV2 *pBuildInfo);
using KWrcDriverStartupFunction = bool (*)(const KWrcDriverHostApiV2 *pHostApi);
using KWrcDriverShutdownFunction = void (*)();

#endif // _WINREMOTECONTROL_WRCDRIVERHOSTAPI_H_
