#ifndef _WINREMOTECONTROL_NATIVECURSOR_H_
#define _WINREMOTECONTROL_NATIVECURSOR_H_

#include "core/media/cursorshape.h"

#include <QtCore/QString>
#include <Windows.h>

// Owns one Windows cursor; source pixels and hotspot remain physical pixels.
class KNativeCursor
{
public:
	KNativeCursor() = default;
	~KNativeCursor();
	KNativeCursor(const KNativeCursor &) = delete;
	KNativeCursor &operator=(const KNativeCursor &) = delete;

	bool initialize(const KCursorShape &shape, QString *pErrorMessage);
	void reset();
	HCURSOR handle() const;
	bool isReady() const;

private:
	HCURSOR m_hCursor = nullptr;
	bool m_bOwned = false;
	bool m_bReady = false;
};

#endif // _WINREMOTECONTROL_NATIVECURSOR_H_
