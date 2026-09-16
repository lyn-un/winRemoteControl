#ifndef _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_
#define _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_

#include <QtCore/QMetaType>
#include <QtCore/QtGlobal>

#include <memory>
#include <vector>
#include <array>
#include <atomic>
#include <limits>

enum class KDecodedPixelFormat
{
	Bgra,
	I420
};

struct KVideoRenderSession
{
	std::atomic<bool> bActive{true};
	std::atomic<bool> bForceBgra{false};
	quint64 nGeneration = 0;
};

struct KVideoPlane
{
	const unsigned char *pData = nullptr;
	int nWidth = 0;
	int nHeight = 0;
	int nStride = 0;
	size_t nBytes = 0;

	bool isValid(int nExpectedWidth, int nExpectedHeight) const
	{
		return pData && nWidth == nExpectedWidth && nHeight == nExpectedHeight && nWidth > 0 && nHeight > 0 &&
			   nStride >= nWidth &&
			   static_cast<size_t>(nHeight - 1) <= ((std::numeric_limits<size_t>::max)() - nWidth) / nStride &&
			   nBytes >= static_cast<size_t>(nHeight - 1) * nStride + nWidth;
	}
};

// Configured before starting media workers; no platform types cross this boundary.
inline std::atomic<bool> g_bForceBgraVideoRenderer{false};

struct KDecodedVideoFrame
{
	int nWidth = 0;
	int nHeight = 0;
	quint64 nFrameIndex = 0;
	quint64 nSourceFrameIndex = 0;
	qint64 nTimestampMs = 0;
	quint64 nLastInputSeq = 0;
	qint64 nInputAgeMs = -1;
	bool bWebRtcLowLatencyRender = false;
	qint64 nRemoteCallbackAtMs = -1;
	qint64 nConversionDoneAtMs = -1;
	qint64 nRenderEnqueuedAtMs = -1;
	std::shared_ptr<std::vector<unsigned char>> spBgraBuffer;
	KDecodedPixelFormat pixelFormat = KDecodedPixelFormat::Bgra;
	std::array<KVideoPlane, 3> planes;
	std::shared_ptr<const void> spPlaneOwner;
	std::shared_ptr<KVideoRenderSession> spRenderSession;
	qint64 nFramePreparedAtMs = -1;

	bool hasPixels() const
	{
		if (nWidth <= 0 || nHeight <= 0 || nWidth > (std::numeric_limits<int>::max)() / 4)
			return false;
		if (spRenderSession && !spRenderSession->bActive.load())
			return false;
		if (pixelFormat == KDecodedPixelFormat::I420)
		{
			return spPlaneOwner && planes[0].isValid(nWidth, nHeight) &&
				   planes[1].isValid(nWidth / 2 + nWidth % 2, nHeight / 2 + nHeight % 2) &&
				   planes[2].isValid(nWidth / 2 + nWidth % 2, nHeight / 2 + nHeight % 2);
		}
		return pixelFormat == KDecodedPixelFormat::Bgra && spBgraBuffer &&
			   static_cast<size_t>(nHeight) <= (std::numeric_limits<size_t>::max)() / (nWidth * 4) &&
			   spBgraBuffer->size() >= static_cast<size_t>(nWidth) * nHeight * 4;
	}

	const unsigned char *pixelData() const
	{
		return pixelFormat == KDecodedPixelFormat::Bgra && hasPixels() ? spBgraBuffer->data() : nullptr;
	}
};

Q_DECLARE_METATYPE(KDecodedVideoFrame)

#endif // _WINREMOTECONTROL_DECODEDVIDEOFRAME_H_
