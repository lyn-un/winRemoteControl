#ifndef _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYPROTOCOL_H_
#define _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYPROTOCOL_H_

#include <cstdint>
#include <array>

namespace KTerminalRelayProtocol
{
	constexpr std::uint32_t kMagic = 0x54435257U; // "WRCT" in little-endian memory.
	constexpr std::uint16_t kVersion = 1;
	constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U;
	constexpr std::size_t kFrameHeaderBytes = 12;
	constexpr std::size_t kResizePayloadBytes = 4;

	enum FrameType : std::uint16_t
	{
		HelloFrameType = 1,
		InputFrameType = 2,
		OutputFrameType = 3,
		ResizeFrameType = 4,
		CloseFrameType = 5
	};
	struct DecodedFrameHeader
	{
		std::uint16_t nType = 0;
		std::uint32_t nPayloadBytes = 0;
	};

	inline void WriteUint16(std::uint8_t *pData, std::uint16_t nValue)
	{
		pData[0] = static_cast<std::uint8_t>(nValue & 0xff);
		pData[1] = static_cast<std::uint8_t>((nValue >> 8) & 0xff);
	}

	inline void WriteUint32(std::uint8_t *pData, std::uint32_t nValue)
	{
		for (int nIndex = 0; nIndex < 4; ++nIndex)
			pData[nIndex] = static_cast<std::uint8_t>((nValue >> (nIndex * 8)) & 0xff);
	}

	inline std::uint16_t ReadUint16(const std::uint8_t *pData)
	{
		return static_cast<std::uint16_t>(pData[0])
			| (static_cast<std::uint16_t>(pData[1]) << 8);
	}

	inline std::uint32_t ReadUint32(const std::uint8_t *pData)
	{
		std::uint32_t nValue = 0;
		for (int nIndex = 0; nIndex < 4; ++nIndex)
			nValue |= static_cast<std::uint32_t>(pData[nIndex]) << (nIndex * 8);
		return nValue;
	}

	inline std::array<std::uint8_t, kFrameHeaderBytes> EncodeFrameHeader(
		std::uint16_t nType, std::uint32_t nPayloadBytes)
	{
		std::array<std::uint8_t, kFrameHeaderBytes> data = {};
		WriteUint32(data.data(), kMagic);
		WriteUint16(data.data() + 4, kVersion);
		WriteUint16(data.data() + 6, nType);
		WriteUint32(data.data() + 8, nPayloadBytes);
		return data;
	}

	inline bool DecodeFrameHeader(const std::uint8_t *pData,
		DecodedFrameHeader *pHeader)
	{
		if (pData == nullptr || pHeader == nullptr
			|| ReadUint32(pData) != kMagic || ReadUint16(pData + 4) != kVersion)
		{
			return false;
		}
		pHeader->nType = ReadUint16(pData + 6);
		pHeader->nPayloadBytes = ReadUint32(pData + 8);
		return true;
	}

	inline std::array<std::uint8_t, kResizePayloadBytes> EncodeResizePayload(
		std::uint16_t nColumns, std::uint16_t nRows)
	{
		std::array<std::uint8_t, kResizePayloadBytes> data = {};
		WriteUint16(data.data(), nColumns);
		WriteUint16(data.data() + 2, nRows);
		return data;
	}

	inline bool IsKnownFrameType(std::uint16_t nType)
	{
		return nType >= HelloFrameType && nType <= CloseFrameType;
	}
}

#endif // _WINREMOTECONTROL_CORE_TERMINAL_TERMINALRELAYPROTOCOL_H_
