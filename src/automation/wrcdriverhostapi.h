#ifndef _WINREMOTECONTROL_WRCDRIVERHOSTAPI_H_
#define _WINREMOTECONTROL_WRCDRIVERHOSTAPI_H_

#include <cstdint>

constexpr std::uint32_t KWrcDriverAbiVersion1 = 1;
constexpr std::uint32_t KWrcDriverArchitectureX64 = 1;
constexpr std::uint32_t KWrcDriverRuntimeDebug = 1;
constexpr std::uint32_t KWrcDriverRuntimeRelease = 2;
inline constexpr char KWrcAutomationBuildId[] = "wrc-automation-abi1-20260828";

using KWrcDriverJsonCallback = void (*)(void *pCallbackContext,
	std::uint64_t nRequestId,
	const char *pJsonUtf8,
	std::uint32_t nJsonBytes);

struct KWrcDriverBuildInfoV1
{
	std::uint32_t nStructSize = sizeof(KWrcDriverBuildInfoV1);
	std::uint32_t nAbiVersion = KWrcDriverAbiVersion1;
	std::uint32_t nArchitecture = KWrcDriverArchitectureX64;
	std::uint32_t nRuntimeFlavor = KWrcDriverRuntimeRelease;
	std::uint32_t nQtMajorVersion = 0;
	char szBuildId[64] = {};
};

struct KWrcDriverHostApiV1
{
	std::uint32_t nStructSize = sizeof(KWrcDriverHostApiV1);
	std::uint32_t nAbiVersion = KWrcDriverAbiVersion1;
	void *pHostContext = nullptr;
	void (*submitCommand)(void *pHostContext,
		std::uint64_t nRequestId,
		const char *pCommandIdUtf8,
		std::uint32_t nCommandIdBytes,
		const char *pArgumentsJsonUtf8,
		std::uint32_t nArgumentsJsonBytes,
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
	void (*writeLog)(void *pHostContext,
		std::uint32_t nLevel,
		const char *pMessageUtf8,
		std::uint32_t nMessageBytes) = nullptr;
};

using KWrcDriverAbiVersionFunction = std::uint32_t (*)();
using KWrcDriverBuildInfoFunction = bool (*)(KWrcDriverBuildInfoV1 *pBuildInfo);
using KWrcDriverStartupFunction = bool (*)(const KWrcDriverHostApiV1 *pHostApi);
using KWrcDriverShutdownFunction = void (*)();

#endif // _WINREMOTECONTROL_WRCDRIVERHOSTAPI_H_
