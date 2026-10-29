#include "render/nativecursor.h"

#include <cstring>

namespace
{
struct KBitmap
{
	HBITMAP hBitmap = nullptr;
	~KBitmap()
	{
		if (hBitmap != nullptr)
			::DeleteObject(hBitmap);
	}
};

struct KMonochromeInfo
{
	BITMAPINFOHEADER header = {};
	RGBQUAD palette[2] = {};
};

bool Fail(const QString &strError, QString *pErrorMessage)
{
	if (pErrorMessage != nullptr)
		*pErrorMessage = strError;
	return false;
}
}

KNativeCursor::~KNativeCursor()
{
	reset();
}

bool KNativeCursor::initialize(const KCursorShape &shape, QString *pErrorMessage)
{
	if (pErrorMessage != nullptr)
		pErrorMessage->clear();
	if (!shape.isValid())
		return Fail(QStringLiteral("Invalid native cursor shape"), pErrorMessage);
	if (shape.kind == DefaultCursorShape || !shape.bVisible)
	{
		HCURSOR hCursor = shape.bVisible ? ::LoadCursorW(nullptr, IDC_ARROW) : nullptr;
		if (shape.bVisible && hCursor == nullptr)
			return Fail(QStringLiteral("Load default cursor failed"), pErrorMessage);
		reset();
		m_hCursor = hCursor;
		m_bReady = true;
		return true;
	}
	const bool bMonochrome = shape.kind == MonochromeCursorShape;
	KBitmap mask;
	void *pMaskBits = nullptr;
	KMonochromeInfo maskInfo;
	maskInfo.header.biSize = sizeof(BITMAPINFOHEADER);
	maskInfo.header.biWidth = shape.nWidth;
	maskInfo.header.biHeight = -shape.nHeight * (bMonochrome ? 2 : 1);
	maskInfo.header.biPlanes = 1;
	maskInfo.header.biBitCount = 1;
	maskInfo.header.biCompression = BI_RGB;
	maskInfo.palette[1] = { 255, 255, 255, 0 };
	mask.hBitmap = ::CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO *>(&maskInfo),
		DIB_RGB_COLORS, &pMaskBits, nullptr, 0);
	if (mask.hBitmap == nullptr || pMaskBits == nullptr)
		return Fail(QStringLiteral("Create cursor mask failed"), pErrorMessage);
	const int nMaskStride = ((shape.nWidth + 31) / 32) * 4;
	std::memset(pMaskBits, 0, nMaskStride * shape.nHeight * (bMonochrome ? 2 : 1));
	KBitmap color;
	void *pColorBits = nullptr;
	if (bMonochrome)
	{
		for (int nRow = 0; nRow < shape.nHeight * 2; ++nRow)
		{
			std::memcpy(static_cast<unsigned char *>(pMaskBits) + nRow * nMaskStride,
				shape.data.constData() + nRow * shape.nStride, (shape.nWidth + 7) / 8);
		}
	}
	else
	{
		BITMAPV5HEADER colorInfo = {};
		colorInfo.bV5Size = sizeof(BITMAPV5HEADER);
		colorInfo.bV5Width = shape.nWidth;
		colorInfo.bV5Height = -shape.nHeight;
		colorInfo.bV5Planes = 1;
		colorInfo.bV5BitCount = 32;
		colorInfo.bV5Compression = BI_BITFIELDS;
		colorInfo.bV5RedMask = 0x00ff0000;
		colorInfo.bV5GreenMask = 0x0000ff00;
		colorInfo.bV5BlueMask = 0x000000ff;
		colorInfo.bV5AlphaMask = 0xff000000;
		color.hBitmap = ::CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO *>(&colorInfo),
			DIB_RGB_COLORS, &pColorBits, nullptr, 0);
		if (color.hBitmap == nullptr || pColorBits == nullptr)
			return Fail(QStringLiteral("Create cursor color bitmap failed"), pErrorMessage);
		for (int y = 0; y < shape.nHeight; ++y)
		{
			const auto *pSource = reinterpret_cast<const unsigned char *>(shape.data.constData() + y * shape.nStride);
			auto *pTarget = static_cast<unsigned char *>(pColorBits) + y * shape.nWidth * 4;
			auto *pMask = static_cast<unsigned char *>(pMaskBits) + y * nMaskStride;
			std::memcpy(pTarget, pSource, shape.nWidth * 4);
			for (int x = 0; x < shape.nWidth; ++x)
			{
				const unsigned char nAlpha = pSource[x * 4 + 3];
				if (shape.kind == MaskedColorCursorShape)
				{
					if (nAlpha != 0 && nAlpha != 255)
						return Fail(QStringLiteral("Masked cursor alpha must be 0 or 255"), pErrorMessage);
					if (nAlpha == 255)
						pMask[x / 8] |= static_cast<unsigned char>(0x80 >> (x % 8));
					// Zero alpha selects Windows' legacy (background AND mask) XOR color path.
					pTarget[x * 4 + 3] = 0;
				}
				else
				{
					if (nAlpha == 0)
						pMask[x / 8] |= static_cast<unsigned char>(0x80 >> (x % 8));
					for (int nChannel = 0; nChannel < 3; ++nChannel)
					{
						if (pSource[x * 4 + nChannel] > nAlpha)
							return Fail(QStringLiteral("Cursor color is not premultiplied"), pErrorMessage);
						// DXGI pixels are premultiplied; the native cursor bitmap is straight BGRA.
						pTarget[x * 4 + nChannel] = nAlpha == 0 ? 0 : static_cast<unsigned char>(
							(pSource[x * 4 + nChannel] * 255u + nAlpha / 2u) / nAlpha);
					}
				}
			}
		}
	}
	ICONINFO info = {};
	info.fIcon = FALSE;
	info.xHotspot = shape.nHotspotX;
	info.yHotspot = shape.nHotspotY;
	info.hbmMask = mask.hBitmap;
	info.hbmColor = color.hBitmap;
	HCURSOR hCursor = static_cast<HCURSOR>(::CreateIconIndirect(&info));
	if (hCursor == nullptr)
		return Fail(QStringLiteral("Create native cursor failed: %1").arg(::GetLastError()), pErrorMessage);
	reset();
	m_hCursor = hCursor;
	m_bOwned = true;
	m_bReady = true;
	return true;
}

void KNativeCursor::reset()
{
	if (m_bOwned && m_hCursor != nullptr)
	{
		if (::GetCursor() == m_hCursor)
			::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
		::DestroyIcon(m_hCursor);
	}
	m_hCursor = nullptr;
	m_bOwned = false;
	m_bReady = false;
}

HCURSOR KNativeCursor::handle() const
{
	return m_hCursor;
}

bool KNativeCursor::isReady() const
{
	return m_bReady;
}
