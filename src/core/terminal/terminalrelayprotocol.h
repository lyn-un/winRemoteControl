#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYPROTOCOL_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYPROTOCOL_H_

#include <cstdint>

namespace KTerminalRelayProtocol
{
	constexpr std::uint32_t kMagic = 0x54435257U; // "WRCT" in little-endian memory.
	constexpr std::uint16_t kVersion = 1;
	constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U;

	enum FrameType : std::uint16_t
	{
		HelloFrameType = 1,
		InputFrameType = 2,
		OutputFrameType = 3,
		ResizeFrameType = 4,
		CloseFrameType = 5
	};

#pragma pack(push, 1)
	struct FrameHeader
	{
		std::uint32_t nMagic = kMagic;
		std::uint16_t nVersion = kVersion;
		std::uint16_t nType = 0;
		std::uint32_t nPayloadBytes = 0;
	};

	struct ResizePayload
	{
		std::uint16_t nColumns = 0;
		std::uint16_t nRows = 0;
	};
#pragma pack(pop)

	static_assert(sizeof(FrameHeader) == 12);
	static_assert(sizeof(ResizePayload) == 4);
}

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYPROTOCOL_H_
