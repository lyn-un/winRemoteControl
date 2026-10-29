#include "core/media/cursorshape.h"

int KCursorShape::expectedByteCount() const
{
	if (kind == DefaultCursorShape)
		return nWidth == 0 && nHeight == 0 && nStride == 0 && nHotspotX == 0 && nHotspotY == 0 ? 0 : -1;
	if (kind != ColorCursorShape && kind != MonochromeCursorShape && kind != MaskedColorCursorShape)
		return -1;
	if (nWidth <= 0 || nWidth > kMaximumDimension || nHeight <= 0 || nHeight > kMaximumDimension
		|| nStride <= 0 || nStride > kMaximumStride
		|| nHotspotX < 0 || nHotspotX >= nWidth || nHotspotY < 0 || nHotspotY >= nHeight)
	{
		return -1;
	}
	const int nMinimumStride = kind == MonochromeCursorShape ? (nWidth + 7) / 8 : nWidth * 4;
	if (nStride < nMinimumStride)
		return -1;
	const qint64 nBytes = static_cast<qint64>(nStride) * nHeight * (kind == MonochromeCursorShape ? 2 : 1);
	return nBytes <= kMaximumBytes ? static_cast<int>(nBytes) : -1;
}

bool KCursorShape::isValid() const
{
	const int nExpected = expectedByteCount();
	return nSequence != 0 && nExpected >= 0 && data.size() == nExpected;
}

bool KCursorShape::hasSameMetadata(const KCursorShape &other) const
{
	return kind == other.kind && nSequence == other.nSequence && nWidth == other.nWidth
		&& nHeight == other.nHeight && nStride == other.nStride && nHotspotX == other.nHotspotX
		&& nHotspotY == other.nHotspotY && bVisible == other.bVisible;
}
