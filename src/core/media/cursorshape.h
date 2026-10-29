#ifndef _WINREMOTECONTROL_CURSORSHAPE_H_
#define _WINREMOTECONTROL_CURSORSHAPE_H_

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>

enum KCursorShapeKind
{
	DefaultCursorShape,
	ColorCursorShape,
	MonochromeCursorShape,
	MaskedColorCursorShape
};

struct KCursorShape
{
	static constexpr int kMaximumDimension = 256;
	static constexpr int kMaximumStride = 2048;
	static constexpr int kMaximumBytes = 256 * 1024;

	KCursorShapeKind kind = DefaultCursorShape;
	quint64 nSequence = 0;
	// Local lifecycle only; never accepted from a peer as an authorization token.
	quint64 nGeneration = 0;
	int nWidth = 0;
	int nHeight = 0;
	int nStride = 0;
	int nHotspotX = 0;
	int nHotspotY = 0;
	bool bVisible = true;
	QByteArray data;

	int expectedByteCount() const;
	bool isValid() const;
	bool hasSameMetadata(const KCursorShape &other) const;
};

Q_DECLARE_METATYPE(KCursorShape)

#endif // _WINREMOTECONTROL_CURSORSHAPE_H_
