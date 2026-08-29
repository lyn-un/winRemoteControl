#include <cstdint>

extern "C" __declspec(dllexport) std::uint32_t wrcDriverAbiVersion()
{
	return 1;
}
