#pragma once

#include <Tracy/Tracy.hpp>
#include <Tracy/TracyD3D11.hpp>

#include <optional>
#include <string_view>

#include "Utils/Macros.h"

/// RAII scope that fans a single pass name to all three instrumentation sinks:
///   1. Internal profiler (GPU timestamp + CPU QPC → Profiling table)
///   2. Tracy CPU zone (always-on when TRACY_ENABLE; not gated on frameAnnotations)
///   3. Tracy GPU zone (always-on when TRACY_ENABLE and a D3D11 context exists)
///   4. RenderDoc/PIX ID3DUserDefinedAnnotation (when frameAnnotations is true)
///
/// Use via the convenience macros at render-pass entry points:
///   CS_GPU_PASS("Feature::PassName");                     // literal name, zero allocation
///   CS_GPU_PASS_SELECT(cond, "A", "B");                   // literal ternary, zero allocation
///   CS_GPU_PASS_DYNAMIC(runtimeName);                     // runtime name, allocates
struct ScopedGpuPass
{
	explicit ScopedGpuPass(std::string_view name);
	ScopedGpuPass(const tracy::SourceLocationData* srcloc, std::string_view name);
	~ScopedGpuPass();

	ScopedGpuPass(const ScopedGpuPass&) = delete;
	ScopedGpuPass& operator=(const ScopedGpuPass&) = delete;
	ScopedGpuPass(ScopedGpuPass&&) = delete;
	ScopedGpuPass& operator=(ScopedGpuPass&&) = delete;

private:
#ifdef TRACY_ENABLE
	std::optional<tracy::ScopedZone> cpuZone;
	std::optional<tracy::D3D11ZoneScope> gpuZone;
#endif
	bool annotationOpen = false;
	bool profilerActive = false;
};

#define CS_GPU_PASS(name)                                                                                                                              \
	static constexpr tracy::SourceLocationData CS_DETAIL_CONCAT(cs_gpu_pass_srcloc_, __LINE__){ name, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; \
	ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { &CS_DETAIL_CONCAT(cs_gpu_pass_srcloc_, __LINE__), name }

/// Two static srclocs, not one: a shared static would latch onto whichever branch
/// evaluated first and never reflect the other one again.
#define CS_GPU_PASS_SELECT(cond, name1, name2)                                                                                                           \
	static constexpr tracy::SourceLocationData CS_DETAIL_CONCAT(cs_gpu_pass_srcloc1_, __LINE__){ name1, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; \
	static constexpr tracy::SourceLocationData CS_DETAIL_CONCAT(cs_gpu_pass_srcloc2_, __LINE__){ name2, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; \
	ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { (cond) ? &CS_DETAIL_CONCAT(cs_gpu_pass_srcloc1_, __LINE__) : &CS_DETAIL_CONCAT(cs_gpu_pass_srcloc2_, __LINE__), (cond) ? std::string_view(name1) : std::string_view(name2) }

#define CS_GPU_PASS_DYNAMIC(name) \
	ScopedGpuPass CS_DETAIL_CONCAT(cs_gpu_pass_, __LINE__) { name }
