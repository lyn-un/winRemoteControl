#include "capture/dxgidesktopduplicator.h"

#include <QtCore/QCoreApplication>

#include <iostream>

// Synthetic metadata only: never initialize desktop duplication or inspect a desktop.
struct KHdrCaptureTestAccess
{
	static void setShape(KDxgiDesktopDuplicator *pCapture, UINT nType, UINT nWidth,
		UINT nHeight, UINT nStride, bool bVisible, const std::vector<unsigned char> &data)
	{
		pCapture->m_nSourceGeneration = 1;
		pCapture->m_pointerPosition.Visible = bVisible;
		pCapture->m_pointerShapeInfo = { nType, nWidth, nHeight, nStride, { 1, 2 } };
		pCapture->m_vecPointerShapeBuffer = data;
		pCapture->queueCursorShape();
	}
	static bool updatePosition(KDxgiDesktopDuplicator *pCapture, int nX, int nY, bool bVisible)
	{
		DXGI_OUTDUPL_FRAME_INFO info = {};
		info.LastMouseUpdateTime.QuadPart = 1;
		info.PointerPosition.Position = { nX, nY };
		info.PointerPosition.Visible = bVisible;
		QString strError;
		return pCapture->updatePointerState(info, &strError);
	}
};

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	auto spPath = std::make_shared<KVideoFramePathState>(CpuVideoFramePath);
	spPath->begin(1);
	spPath->setSeparateCursor(1, true);
	KDxgiDesktopDuplicator capture(AutoHdrConversionMode, spPath);
	KCursorShape shape;
	std::vector<unsigned char> mono(4 * 24, 0x55);
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 17, 24, 4, true, mono);
	if (!capture.takeCursorUpdate(&shape) || !shape.isValid() || shape.nWidth != 17 || shape.nHeight != 12
		|| shape.nSequence != 1 || shape.nGeneration != 1 || shape.kind != MonochromeCursorShape
		|| capture.takeCursorUpdate(&shape))
	{
		return 1;
	}
	if (!KHdrCaptureTestAccess::updatePosition(&capture, 30, 40, true) || capture.takeCursorUpdate(&shape))
		return 1;
	if (!KHdrCaptureTestAccess::updatePosition(&capture, 31, 41, false) || !capture.takeCursorUpdate(&shape)
		|| shape.nSequence != 2 || shape.bVisible || shape.data.size() != 96)
	{
		return 1;
	}
	// Coalesce two shape changes before consumption; keep only the latest.
	std::vector<unsigned char> color(8 * 8 * 4, 0x77);
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 8, 8, 32, true, color);
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 8, 8, 32, true, color);
	if (!capture.takeCursorUpdate(&shape) || shape.nSequence != 4 || shape.kind != MaskedColorCursorShape)
		return 1;
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, 8, 8, 32, true, color);
	if (capture.takeCursorUpdate(&shape))
		return 1;
	// Unsupported dimensions and malformed monochrome height become default arrows.
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 10000, 8, 32, true, color);
	if (!capture.takeCursorUpdate(&shape) || !shape.isValid() || shape.kind != DefaultCursorShape)
		return 1;
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME, 17, 23, 4, false, mono);
	if (!capture.takeCursorUpdate(&shape) || shape.kind != DefaultCursorShape || shape.bVisible)
		return 1;
	KHdrCaptureTestAccess::setShape(&capture, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR, 8, 8, 32, true, color);
	spPath->begin(2);
	spPath->setSeparateCursor(2, true);
	if (capture.takeCursorUpdate(&shape))
		return 1;
	capture.shutdown();
	if (capture.takeCursorUpdate(&shape))
		return 1;
	std::cout << "capture cursor: no position echo, visibility, masks, coalescing, bounds and stale generation passed"
		<< std::endl;
	return 0;
}
