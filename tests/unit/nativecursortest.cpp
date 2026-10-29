#include "render/nativecursor.h"

#include <QtCore/QCoreApplication>

#include <array>
#include <cstring>
#include <iostream>

namespace
{
bool DrawAndCheck(const KCursorShape &shape, const std::array<DWORD, 4> &expected)
{
	KNativeCursor cursor;
	QString strError;
	if (!cursor.initialize(shape, &strError))
	{
		std::cerr << strError.toStdString() << std::endl;
		return false;
	}
	ICONINFO info = {};
	if (!::GetIconInfo(cursor.handle(), &info))
		return false;
	const bool bHotspot = !info.fIcon && info.xHotspot == shape.nHotspotX && info.yHotspot == shape.nHotspotY;
	::DeleteObject(info.hbmMask);
	if (info.hbmColor != nullptr)
		::DeleteObject(info.hbmColor);
	BITMAPINFO bitmapInfo = {};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = 4;
	bitmapInfo.bmiHeader.biHeight = -1;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	void *pPixels = nullptr;
	HBITMAP hBitmap = ::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &pPixels, nullptr, 0);
	HDC hContext = ::CreateCompatibleDC(nullptr);
	if (hBitmap == nullptr || hContext == nullptr)
		return false;
	HGDIOBJ hPrevious = ::SelectObject(hContext, hBitmap);
	std::array<DWORD, 4> pixels = { 0x123456, 0x123456, 0x123456, 0x123456 };
	std::memcpy(pPixels, pixels.data(), sizeof(pixels));
	const bool bDrawn = ::DrawIconEx(hContext, 0, 0, cursor.handle(), shape.nWidth, shape.nHeight,
		0, nullptr, DI_NORMAL) != FALSE;
	::GdiFlush();
	std::memcpy(pixels.data(), pPixels, sizeof(pixels));
	::SelectObject(hContext, hPrevious);
	::DeleteDC(hContext);
	::DeleteObject(hBitmap);
	for (size_t nIndex = 0; nIndex < pixels.size(); ++nIndex)
	{
		if ((pixels[nIndex] & 0xffffff) != expected[nIndex])
		{
			std::cerr << "pixel " << nIndex << " actual=" << std::hex << pixels[nIndex]
				<< " expected=" << expected[nIndex] << std::endl;
			return false;
		}
	}
	return bHotspot && bDrawn;
}
}

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	KCursorShape shape;
	shape.nSequence = 1;
	shape.nWidth = 4;
	shape.nHeight = 1;
	shape.nStride = 1;
	shape.nHotspotX = 1;
	shape.kind = MonochromeCursorShape;
	shape.data = QByteArray::fromHex("3050");
	if (!DrawAndCheck(shape, { 0, 0xffffff, 0x123456, 0xedcba9 }))
		return 1;
	shape.kind = MaskedColorCursorShape;
	shape.nStride = 16;
	shape.data = QByteArray::fromHex("554433005500ffff000000ff00000000");
	if (!DrawAndCheck(shape, { 0x334455, 0xed3403, 0x123456, 0 }))
		return 1;
	shape.kind = ColorCursorShape;
	shape.data = QByteArray::fromHex("1020308000000000ffffffFF000000FF");
	// Premultiplied color + background * (255 - alpha) / 255 (Windows rounds).
	if (!DrawAndCheck(shape, { 0x393a3b, 0x123456, 0xffffff, 0 }))
		return 1;
	KNativeCursor cursor;
	QString strError;
	shape.nWidth = 0;
	if (cursor.initialize(shape, &strError))
		return 1;
	shape = {};
	shape.nSequence = 1;
	if (!cursor.initialize(shape, &strError) || !cursor.isReady() || cursor.handle() == nullptr)
		return 1;
	shape.bVisible = false;
	if (!cursor.initialize(shape, &strError) || !cursor.isReady() || cursor.handle() != nullptr)
		return 1;
	cursor.reset();
	if (cursor.isReady())
		return 1;
	shape.kind = MonochromeCursorShape;
	shape.nWidth = 4;
	shape.nHeight = 1;
	shape.nStride = 1;
	shape.bVisible = true;
	shape.data = QByteArray::fromHex("3050");
	const DWORD nUserObjects = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
	const DWORD nGdiObjects = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	for (int nIndex = 0; nIndex < 200; ++nIndex)
	{
		if (!cursor.initialize(shape, &strError))
			return 1;
		cursor.reset();
	}
	if (::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS) > nUserObjects + 2
		|| ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS) > nGdiObjects + 2)
	{
		return 1;
	}
	std::cout << "native cursor: monochrome AND/XOR, masked color, premultiplied alpha, hotspot and reset passed"
		<< std::endl;
	return 0;
}
