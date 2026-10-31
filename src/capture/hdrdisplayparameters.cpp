#include "capture/hdrdisplayparameters.h"

#include <cwchar>
#include <vector>

LONG QueryHdrSdrWhiteLevel(const wchar_t *pDeviceName, ULONG *pWhiteLevel)
{
	if (!pDeviceName || !*pDeviceName || !pWhiteLevel)
		return ERROR_INVALID_PARAMETER;
	*pWhiteLevel = 0;
	// Display topology may change between sizing and querying; retry only that race.
	for (int nAttempt = 0; nAttempt < 3; ++nAttempt)
	{
		UINT32 nPathCount = 0;
		UINT32 nModeCount = 0;
		LONG nError = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPathCount, &nModeCount);
		if (nError != ERROR_SUCCESS)
			return nError;
		std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModeCount);
		nError = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPathCount, paths.data(),
			&nModeCount, modes.data(), nullptr);
		if (nError == ERROR_INSUFFICIENT_BUFFER)
			continue;
		if (nError != ERROR_SUCCESS)
			return nError;
		bool bMatched = false;
		ULONG nMatchedLevel = 0;
		for (UINT32 nIndex = 0; nIndex < nPathCount; ++nIndex)
		{
			const auto &path = paths[nIndex];
			DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
			source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			source.header.size = sizeof(source);
			source.header.adapterId = path.sourceInfo.adapterId;
			source.header.id = path.sourceInfo.id;
			nError = DisplayConfigGetDeviceInfo(&source.header);
			if (nError != ERROR_SUCCESS)
				return nError;
			if (_wcsicmp(source.viewGdiDeviceName, pDeviceName) != 0)
				continue;
			// A cloned source can address multiple targets. Do not guess which white wins.
			if (bMatched)
				return ERROR_NOT_SUPPORTED;
			DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
			white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
			white.header.size = sizeof(white);
			white.header.adapterId = path.targetInfo.adapterId;
			white.header.id = path.targetInfo.id;
			nError = DisplayConfigGetDeviceInfo(&white.header);
			if (nError != ERROR_SUCCESS)
				return nError;
			if (white.SDRWhiteLevel == 0)
				return ERROR_INVALID_DATA;
			nMatchedLevel = white.SDRWhiteLevel;
			bMatched = true;
		}
		if (!bMatched)
			return ERROR_NOT_FOUND;
		*pWhiteLevel = nMatchedLevel;
		return ERROR_SUCCESS;
	}
	return ERROR_INSUFFICIENT_BUFFER;
}
