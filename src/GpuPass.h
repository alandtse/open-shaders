#pragma once

#include <Tracy/TracyC.h>
#include <Tracy/TracyD3D11.hpp>

#include <optional>
#include <string_view>

/// RAII scope that fans a single pass name to all three instrumentation sinks:
///   1. Internal profiler (GPU timestamp + CPU QPC → Profiling table)
///   2. Tracy CPU zone (always-on when TRACY_ENABLE; not gated on frameAnnotations)
///   3. Tracy GPU zone (always-on when TRACY_ENABLE and a D3D11 context exists)
///   4. RenderDoc/PIX ID3DUserDefinedAnnotation (when frameAnnotations is true)
///
/// Use via the convenience macro at render-pass entry points:
///   CS_GPU_PASS("Feature::PassName");
struct ScopedGpuPass
{
	explicit ScopedGpuPass(std::string_view name);
	~ScopedGpuPass();

	ScopedGpuPass(const ScopedGpuPass&) = delete;
	ScopedGpuPass& operator=(const ScopedGpuPass&) = delete;
	ScopedGpuPass(ScopedGpuPass&&) = delete;
	ScopedGpuPass& operator=(ScopedGpuPass&&) = delete;

private:
#ifdef TRACY_ENABLE
	TracyCZoneCtx cpuZoneCtx{};
	std::optional<tracy::D3D11ZoneScope> gpuZone;
#endif
	bool annotationOpen = false;
	bool profilerActive = false;
};

#define CS_GPU_PASS_CONCAT_IMPL(a, b) a##b
#define CS_GPU_PASS_CONCAT(a, b) CS_GPU_PASS_CONCAT_IMPL(a, b)

/// Drop a single-line GPU pass scope at render-pass entry.
/// The variable name is mangled with __LINE__ so two CS_GPU_PASS calls in the
/// same function cannot collide.
#define CS_GPU_PASS(name) \
	ScopedGpuPass CS_GPU_PASS_CONCAT(cs_gpu_pass_, __LINE__) { name }
