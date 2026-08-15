#include "ShadowParabolicNullAccumulatorFix.h"

void ShadowParabolicNullAccumulatorFix::Install()
{
	stl::detour_thunk<BSParabolicCullingProcess_SetBackHemisphereAccumulator>(REL::RelocationID(101600, 108603));
}

void ShadowParabolicNullAccumulatorFix::BSParabolicCullingProcess_SetBackHemisphereAccumulator::thunk(void* a_this, void* a_accumulator)
{
	if (!a_this) {
		static std::atomic<int> n{ 0 };
		if (n.fetch_add(1, std::memory_order_relaxed) < 20)
			logger::warn("[EngineFixes] Skipped SetBackHemisphereAccumulator on a null culling process (light not yet initialized)");
		return;
	}
	func(a_this, a_accumulator);
}
