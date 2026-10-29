#include "core/media/videoframepath.h"

#include <iostream>

int main()
{
	KVideoFramePathState automatic(AutoVideoFramePath);
	automatic.begin(1);
	automatic.setSeparateCursor(1, true);
	automatic.selectGpu(1);
	automatic.begin(1);
	if (automatic.snapshot().decision != KVideoFramePathState::Gpu || !automatic.snapshot().bSeparateCursor)
		return 1;
	automatic.begin(2);
	automatic.fallback(1, QStringLiteral("stale"));
	automatic.setSeparateCursor(1, true);
	if (automatic.snapshot().decision != KVideoFramePathState::Undecided || automatic.snapshot().bSeparateCursor)
		return 1;
	KVideoFramePathState running(AutoVideoFramePath);
	running.begin(1);
	running.selectGpu(1);
	running.fallback(1, QStringLiteral("unexpected_runtime_mapping"));
	if (running.snapshot().decision != KVideoFramePathState::Failed)
		return 1;
	automatic.fallback(2, QStringLiteral("cross_adapter"));
	automatic.selectGpu(2);
	if (automatic.snapshot().decision != KVideoFramePathState::Cpu
		|| automatic.snapshot().strReason != QStringLiteral("cross_adapter"))
		return 1;
	KVideoFramePathState forced(GpuVideoFramePath);
	forced.begin(5);
	forced.fallback(5, QStringLiteral("initialization_failed"));
	forced.selectGpu(5);
	if (forced.snapshot().decision != KVideoFramePathState::Failed)
		return 1;
	forced.begin(6);
	forced.selectGpu(6);
	if (forced.snapshot().decision != KVideoFramePathState::Gpu)
		return 1;
	KVideoFramePathState cpu(CpuVideoFramePath);
	cpu.begin(1);
	cpu.selectGpu(1);
	if (cpu.snapshot().decision != KVideoFramePathState::Cpu)
		return 1;
	std::cout << "frame path: generation, latched fallback, forced error, next-session retry passed" << std::endl;
	return 0;
}
