#include "GpuPass.h"

#include "Globals.h"
#include "State.h"

ScopedGpuPass::ScopedGpuPass(const tracy::SourceLocationData* srcloc, std::string_view name)
{
	auto* profiler = globals::profiler;
	auto* state = globals::state;

	// 1. Internal profiler: GPU timestamp query start + always-on CPU QPC.
	//    BeginPass also fires the legacy BeginPerfEvent callback for any
	//    call sites that are not yet migrated to ScopedGpuPass.
	// Gates EndPass below: at capacity BeginPass returns false and opens
	// nothing, so EndPass must not close an unrelated already-open pass.
	if (profiler)
		profilerActive = profiler->BeginPass(name, false);

#ifdef TRACY_ENABLE
	// 2. Tracy CPU zone — static srcloc built by the caller's macro, zero allocation.
	cpuZone.emplace(srcloc, -1, true);

	// 3. Tracy GPU zone — requires a D3D11 context from State. Static srcloc path.
	if (state && state->tracyCtx) {
		gpuZone.emplace(state->tracyCtx, srcloc, true);
	}
#endif

	// 4. RenderDoc/PIX annotation — gated on frameAnnotations.
	//    Calls BeginAnnotation (pPerf-only, no Tracy) to avoid double-emitting
	//    the Tracy CPU zone that BeginPerfEvent would add.
	if (state && state->frameAnnotations) {
		state->BeginAnnotation(name);
		annotationOpen = true;
	}
}

ScopedGpuPass::ScopedGpuPass(std::string_view name)
{
	auto* profiler = globals::profiler;
	auto* state = globals::state;

	// 1. Internal profiler: GPU timestamp query start + always-on CPU QPC.
	if (profiler)
		profilerActive = profiler->BeginPass(name, false);

#ifdef TRACY_ENABLE
	// 2. Tracy CPU zone — dynamic (transient) name path, kept for runtime-computed
	//    names. Allocates a srcloc buffer per call; freed by Tracy after serialization.
	cpuZone.emplace(
		uint32_t(0),
		"GpuPass", sizeof("GpuPass") - 1,
		"ScopedGpuPass", sizeof("ScopedGpuPass") - 1,
		name.data(), name.size(),
		uint32_t(0), -1, true);

	// 3. Tracy GPU zone — requires a D3D11 context from State. Use the raw
	//    source-location overload for a dynamic (transient) zone name; the bundled
	//    Tracy dropped the alloc'd-uint64-srcloc D3D11ZoneScope overload. depth=0.
	if (state && state->tracyCtx) {
		gpuZone.emplace(state->tracyCtx,
			uint32_t(0),
			"GpuPass", sizeof("GpuPass") - 1,
			"ScopedGpuPass", sizeof("ScopedGpuPass") - 1,
			name.data(), name.size(),
			0, true);
	}
#endif

	// 4. RenderDoc/PIX annotation — gated on frameAnnotations.
	//    Calls BeginAnnotation (pPerf-only, no Tracy) to avoid double-emitting
	//    the Tracy CPU zone that BeginPerfEvent would add.
	if (state && state->frameAnnotations) {
		state->BeginAnnotation(name);
		annotationOpen = true;
	}
}

ScopedGpuPass::~ScopedGpuPass()
{
	auto* state = globals::state;

	if (annotationOpen && state)
		state->EndAnnotation();

#ifdef TRACY_ENABLE
	gpuZone.reset();
	cpuZone.reset();
#endif

	if (profilerActive)
		globals::profiler->EndPass(false);
}
