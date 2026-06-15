#include "common/framewatermark.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr int kBytesPerPixel = 4;
	constexpr int kBlockSize = 3;
	constexpr int kColumns = 36;
	constexpr int kRows = 3;
	constexpr int kBitCount = kColumns * kRows;
	constexpr int kWatermarkWidth = kColumns * kBlockSize;
	constexpr int kWatermarkHeight = kRows * kBlockSize;
	constexpr quint16 kWatermarkMagic = 0x5743; // WC
	constexpr quint8 kWatermarkVersion = 1;
	constexpr int kMagicBitOffset = 0;
	constexpr int kMagicBitCount = 16;
	constexpr int kVersionBitOffset = kMagicBitOffset + kMagicBitCount;
	constexpr int kVersionBitCount = 4;
	constexpr int kSourceFrameBitOffset = kVersionBitOffset + kVersionBitCount;
	constexpr int kSourceFrameBitCount = 32;
	constexpr int kInputSeqBitOffset = kSourceFrameBitOffset + kSourceFrameBitCount;
	constexpr int kInputSeqBitCount = 32;
	constexpr int kInputAgeBitOffset = kInputSeqBitOffset + kInputSeqBitCount;
	constexpr int kInputAgeBitCount = 16;
	constexpr int kChecksumBitOffset = kInputAgeBitOffset + kInputAgeBitCount;
	constexpr int kChecksumBitCount = 8;
	constexpr int kBitBrightnessThreshold = 128;

	static bool isValidFrameSize(int nWidth, int nHeight, size_t nBufferSize)
	{
		return nWidth >= kWatermarkWidth
			&& nHeight >= kWatermarkHeight
			&& nBufferSize >= static_cast<size_t>(nWidth) * static_cast<size_t>(nHeight) * kBytesPerPixel;
	}

	static quint8 checksum(quint16 nMagic,
		quint8 nVersion,
		quint32 nSourceFrame,
		quint32 nInputSeq,
		quint16 nInputAge)
	{
		quint32 nChecksum = 0x5a;
		nChecksum += static_cast<quint8>(nMagic & 0xff);
		nChecksum += static_cast<quint8>((nMagic >> 8) & 0xff);
		nChecksum += nVersion;
		for (int i = 0; i < 4; ++i)
			nChecksum += static_cast<quint8>((nSourceFrame >> (i * 8)) & 0xff);
		for (int i = 0; i < 4; ++i)
			nChecksum += static_cast<quint8>((nInputSeq >> (i * 8)) & 0xff);
		nChecksum += static_cast<quint8>(nInputAge & 0xff);
		nChecksum += static_cast<quint8>((nInputAge >> 8) & 0xff);
		return static_cast<quint8>(nChecksum & 0xff);
	}

	static void setBits(std::array<bool, kBitCount> *pBits, int nOffset, quint64 nValue, int nCount)
	{
		for (int i = 0; i < nCount; ++i)
			(*pBits)[nOffset + i] = ((nValue >> i) & 1) != 0;
	}

	static quint64 readBits(const std::array<bool, kBitCount> &bits, int nOffset, int nCount)
	{
		quint64 nValue = 0;
		for (int i = 0; i < nCount; ++i)
		{
			if (bits[nOffset + i])
				nValue |= static_cast<quint64>(1) << i;
		}
		return nValue;
	}

	static void writeBlock(std::vector<unsigned char> *pBgraBuffer,
		int nWidth,
		int nStartX,
		int nStartY,
		bool bValue)
	{
		const unsigned char nColor = bValue ? 255 : 0;
		for (int y = 0; y < kBlockSize; ++y)
		{
			for (int x = 0; x < kBlockSize; ++x)
			{
				unsigned char *pPixel = pBgraBuffer->data()
					+ (static_cast<size_t>(nStartY + y) * static_cast<size_t>(nWidth)
						+ static_cast<size_t>(nStartX + x)) * kBytesPerPixel;
				pPixel[0] = nColor;
				pPixel[1] = nColor;
				pPixel[2] = nColor;
				pPixel[3] = 255;
			}
		}
	}

	static bool readBlock(const std::vector<unsigned char> &bgraBuffer,
		int nWidth,
		int nStartX,
		int nStartY)
	{
		int nSum = 0;
		for (int y = 0; y < kBlockSize; ++y)
		{
			for (int x = 0; x < kBlockSize; ++x)
			{
				const unsigned char *pPixel = bgraBuffer.data()
					+ (static_cast<size_t>(nStartY + y) * static_cast<size_t>(nWidth)
						+ static_cast<size_t>(nStartX + x)) * kBytesPerPixel;
				nSum += static_cast<int>(pPixel[0]);
				nSum += static_cast<int>(pPixel[1]);
				nSum += static_cast<int>(pPixel[2]);
			}
		}

		const int nSampleCount = kBlockSize * kBlockSize * 3;
		return nSum / nSampleCount >= kBitBrightnessThreshold;
	}
}

bool KFrameWatermarkCodec::writeBgra(std::vector<unsigned char> *pBgraBuffer,
	int nWidth,
	int nHeight,
	const KFrameWatermark &watermark)
{
	if (pBgraBuffer == nullptr || !isValidFrameSize(nWidth, nHeight, pBgraBuffer->size()))
		return false;

	const quint32 nSourceFrame = static_cast<quint32>(watermark.nSourceFrameIndex & 0xffffffffu);
	const quint32 nInputSeq = static_cast<quint32>(watermark.nLastInputSeq & 0xffffffffu);
	const quint16 nInputAge =
		static_cast<quint16>(std::clamp<qint64>(watermark.nInputAgeMs, 0, std::numeric_limits<quint16>::max()));
	const quint8 nChecksum = checksum(kWatermarkMagic,
		kWatermarkVersion,
		nSourceFrame,
		nInputSeq,
		nInputAge);

	std::array<bool, kBitCount> bits = {};
	setBits(&bits, kMagicBitOffset, kWatermarkMagic, kMagicBitCount);
	setBits(&bits, kVersionBitOffset, kWatermarkVersion, kVersionBitCount);
	setBits(&bits, kSourceFrameBitOffset, nSourceFrame, kSourceFrameBitCount);
	setBits(&bits, kInputSeqBitOffset, nInputSeq, kInputSeqBitCount);
	setBits(&bits, kInputAgeBitOffset, nInputAge, kInputAgeBitCount);
	setBits(&bits, kChecksumBitOffset, nChecksum, kChecksumBitCount);

	const int nOriginX = nWidth - kWatermarkWidth;
	const int nOriginY = nHeight - kWatermarkHeight;
	for (int i = 0; i < kBitCount; ++i)
	{
		const int nColumn = i % kColumns;
		const int nRow = i / kColumns;
		writeBlock(pBgraBuffer,
			nWidth,
			nOriginX + nColumn * kBlockSize,
			nOriginY + nRow * kBlockSize,
			bits[i]);
	}

	return true;
}

bool KFrameWatermarkCodec::readBgra(const std::vector<unsigned char> &bgraBuffer,
	int nWidth,
	int nHeight,
	KFrameWatermark *pWatermark)
{
	if (pWatermark == nullptr || !isValidFrameSize(nWidth, nHeight, bgraBuffer.size()))
		return false;

	std::array<bool, kBitCount> bits = {};
	const int nOriginX = nWidth - kWatermarkWidth;
	const int nOriginY = nHeight - kWatermarkHeight;
	for (int i = 0; i < kBitCount; ++i)
	{
		const int nColumn = i % kColumns;
		const int nRow = i / kColumns;
		bits[i] = readBlock(bgraBuffer,
			nWidth,
			nOriginX + nColumn * kBlockSize,
			nOriginY + nRow * kBlockSize);
	}

	const quint16 nMagic = static_cast<quint16>(readBits(bits, kMagicBitOffset, kMagicBitCount));
	const quint8 nVersion = static_cast<quint8>(readBits(bits, kVersionBitOffset, kVersionBitCount));
	const quint32 nSourceFrame = static_cast<quint32>(readBits(bits, kSourceFrameBitOffset, kSourceFrameBitCount));
	const quint32 nInputSeq = static_cast<quint32>(readBits(bits, kInputSeqBitOffset, kInputSeqBitCount));
	const quint16 nInputAge = static_cast<quint16>(readBits(bits, kInputAgeBitOffset, kInputAgeBitCount));
	const quint8 nChecksum = static_cast<quint8>(readBits(bits, kChecksumBitOffset, kChecksumBitCount));

	if (nMagic != kWatermarkMagic
		|| nVersion != kWatermarkVersion
		|| nChecksum != checksum(nMagic, nVersion, nSourceFrame, nInputSeq, nInputAge))
		return false;

	pWatermark->nSourceFrameIndex = nSourceFrame;
	pWatermark->nLastInputSeq = nInputSeq;
	pWatermark->nInputAgeMs = nInputAge;
	return true;
}
