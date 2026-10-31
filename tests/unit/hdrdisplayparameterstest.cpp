#include "capture/hdrdisplayparameters.h"

#include <cmath>
#include <iostream>

int main()
{
	KHdrWhiteLevelState state;
	if (state.isMeasured() || state.scale() != 2.5f
		|| state.update(ERROR_NOT_FOUND, 1000) || state.update(ERROR_SUCCESS, 0))
		return 1;
	for (ULONG nLevel : { 1000UL, 2000UL, 2500UL, 4000UL })
	{
		state.update(ERROR_SUCCESS, nLevel);
		if (!state.isMeasured() || std::abs(state.scale() * 80.0f - nLevel * 0.08f) > 0.001f
			|| state.update(ERROR_SUCCESS, nLevel))
			return 1;
		const float fBefore = state.scale();
		if (state.update(ERROR_ACCESS_DENIED, 1000) || state.scale() != fBefore)
			return 1;
	}
	state = {};
	if (state.isMeasured() || state.scale() != 2.5f)
		return 1;
	ULONG nLevel = 123;
	if (QueryHdrSdrWhiteLevel(nullptr, &nLevel) != ERROR_INVALID_PARAMETER
		|| QueryHdrSdrWhiteLevel(L"", &nLevel) != ERROR_INVALID_PARAMETER
		|| QueryHdrSdrWhiteLevel(L"invalid", nullptr) != ERROR_INVALID_PARAMETER)
		return 1;
	std::cout << "PASS white level conversion, retention, reset and invalid query arguments\n";
	return 0;
}
