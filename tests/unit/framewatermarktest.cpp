#include "common/framewatermark.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

#include <algorithm>
#include <vector>

namespace
{
	constexpr int kWidth = 128;
	constexpr int kHeight = 48;
	constexpr int kBytesPerPixel = 4;
	constexpr int kCleanupWidth = 112;
	constexpr int kCleanupHeight = 16;

	int g_nFailureCount = 0;

	void check(bool bCondition, const QString &strDescription)
	{
		if (bCondition)
			return;
		qCritical().noquote() << QStringLiteral("FAILED: %1").arg(strDescription);
		++g_nFailureCount;
	}

	void testWatermarkRoundTripAndRemoval()
	{
		std::vector<unsigned char> bgraBuffer(
			static_cast<size_t>(kWidth) * kHeight * kBytesPerPixel);
		for (int y = 0; y < kHeight; ++y)
		{
			for (int x = 0; x < kWidth; ++x)
			{
				unsigned char *pPixel = bgraBuffer.data()
					+ (static_cast<size_t>(y) * kWidth + x) * kBytesPerPixel;
				pPixel[0] = static_cast<unsigned char>(32 + y);
				pPixel[1] = static_cast<unsigned char>(64 + y);
				pPixel[2] = static_cast<unsigned char>(96 + y);
				pPixel[3] = 255;
			}
		}

		KFrameWatermark source;
		source.nSourceFrameIndex = 42;
		source.nLastInputSeq = 73;
		source.nInputAgeMs = 18;
		check(KFrameWatermarkCodec::writeBgra(&bgraBuffer, kWidth, kHeight, source),
			QStringLiteral("watermark is written"));

		KFrameWatermark decoded;
		check(KFrameWatermarkCodec::readBgra(bgraBuffer, kWidth, kHeight, &decoded)
			&& decoded.nSourceFrameIndex == source.nSourceFrameIndex
			&& decoded.nLastInputSeq == source.nLastInputSeq
			&& decoded.nInputAgeMs == source.nInputAgeMs,
			QStringLiteral("watermark metadata survives the frame"));
		check(KFrameWatermarkCodec::removeBgra(&bgraBuffer, kWidth, kHeight),
			QStringLiteral("visible watermark is removed"));

		const int nOriginX = kWidth - kCleanupWidth;
		for (int y = 0; y < kCleanupHeight; ++y)
		{
			const size_t nSourceOffset = (static_cast<size_t>(kHeight - 2 * kCleanupHeight + y)
				* kWidth + nOriginX) * kBytesPerPixel;
			const size_t nDestinationOffset = (static_cast<size_t>(kHeight - kCleanupHeight + y)
				* kWidth + nOriginX) * kBytesPerPixel;
			check(std::equal(bgraBuffer.begin() + static_cast<ptrdiff_t>(nSourceOffset),
				bgraBuffer.begin() + static_cast<ptrdiff_t>(nSourceOffset + kCleanupWidth * kBytesPerPixel),
				bgraBuffer.begin() + static_cast<ptrdiff_t>(nDestinationOffset)),
				QStringLiteral("cleanup area is restored from adjacent pixels"));
		}
	}
}

int main(int nArgc, char *pArgv[])
{
	QCoreApplication application(nArgc, pArgv);
	testWatermarkRoundTripAndRemoval();
	if (g_nFailureCount == 0)
		qInfo() << "All frame watermark tests passed";
	return g_nFailureCount == 0 ? 0 : 1;
}
