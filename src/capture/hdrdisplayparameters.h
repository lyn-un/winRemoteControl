#ifndef _WINREMOTECONTROL_HDRDISPLAYPARAMETERS_H_
#define _WINREMOTECONTROL_HDRDISPLAYPARAMETERS_H_

#include <Windows.h>

// Query only: never acquires desktop pixels or changes display settings.
LONG QueryHdrSdrWhiteLevel(const wchar_t *pDeviceName, ULONG *pWhiteLevel);

class KHdrWhiteLevelState
{
public:
	bool update(LONG nError, ULONG nWhiteLevel)
	{
		if (nError != ERROR_SUCCESS || nWhiteLevel == 0)
			return false;
		const float fScale = static_cast<float>(nWhiteLevel) / 1000.0f;
		const bool bChanged = !m_bMeasured || fScale != m_fScale;
		m_fScale = fScale;
		m_bMeasured = true;
		return bChanged;
	}
	float scale() const { return m_fScale; }
	bool isMeasured() const { return m_bMeasured; }

private:
	float m_fScale = 2.5f;
	bool m_bMeasured = false;
};

#endif // _WINREMOTECONTROL_HDRDISPLAYPARAMETERS_H_
