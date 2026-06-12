#include "GpuPass.h"

#include "Globals.h"
#include "State.h"

ScopedGpuPass::ScopedGpuPass(std::string_view name)
{
	auto* profiler = globals::profiler;
	auto* state = globals::state;

	// 1. Internal profiler: GPU timestamp query start + always-on CPU QPC.
	//    BeginPass also fires the legacy BeginPerfEvent callback for any
	//    call sites that are not yet migrated to ScopedGpuPass.
	if (profiler) {
		profiler->BeginPass(std::string(name), false);
		profilerActive = true;
	}

#ifdef TRACY_ENABLE
	// 2. Tracy CPU zone — unconditional; not gated on frameAnnotations so
	//    CPU profiles are available in any TRACY_SUPPORT build.
	{
		const auto srcloc = ___tracy_alloc_srcloc_name(
			0,
			"GpuPass", sizeof("GpuPass") - 1,
			"ScopedGpuPass", sizeof("ScopedGpuPass") - 1,
			name.data(), name.size(),
			0);
		cpuZoneCtx = ___tracy_emit_zone_begin_alloc(srcloc, true);
	}

	// 3. Tracy GPU zone — requires a D3D11 context from State.
	if (state && state->tracyCtx) {
		const auto srcloc = ___tracy_alloc_srcloc_name(
			0,
			"GpuPass", sizeof("GpuPass") - 1,
			"ScopedGpuPass", sizeof("ScopedGpuPass") - 1,
			name.data(), name.size(),
			0);
		gpuZone.emplace(state->tracyCtx, srcloc, 0, true);
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
	TracyCZoneEnd(cpuZoneCtx);
#endif

	if (profilerActive)
		globals::profiler->EndPass(false);
}
