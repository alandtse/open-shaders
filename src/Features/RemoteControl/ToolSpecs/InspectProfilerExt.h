#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for inspect extension profiler. {brand} is expanded at registration. */
	inline constexpr const char* kInspectProfilerExt =
		R"({"description":")"
		R"({brand} GPU/CPU profiler snapshot -> {enabled,capturing,frame_count,capturedFrameCount,capturedGpuFrameCount,capturedCpuFrameCount,totalMs,cpuTotalMs,resolvedTotalMs,resolvedCpuTotalMs,acquiredSlots,)"
		R"(peakAcquiredSlots,slotRefusals,maxTimers,timers:[{name,gpuMs,topLevelMs,avgMs,p95Ms,p99Ms,cpuMs,cpuAvgMs,cpuP95Ms,cpuP99Ms,hasGpu,hasCpu,activeGpu,activeCpu,historyHead,historyCount,cpuHistoryHead,)"
		R"(cpuHistoryCount}]}. Calling this primes the next capture, )"
		R"(so an idle first call may show stale data. CPU samples publish at their engine-frame boundary and pair with capturedCpuFrameCount, resolvedCpuTotalMs, cpuMs, )"
		R"(and CPU history. GPU samples resolve independently through a three-slot query ring (a frame that cannot open at its first pass, because its oldest slot is still pending or its batch fails to begin, is skipped whole, so a GPU-bound scene publishes fewer, complete GPU samples) and pair with capturedGpuFrameCount, resolvedTotalMs, gpuMs, topLevelMs, )"
		R"(and GPU history. gpuMs and its rolling statistics report self time with directly nested GPU passes removed. topLevelMs is the inclusive contribution from top-level intervals; )"
		R"(resolvedTotalMs equals the sum of topLevelMs across timers. capturedFrameCount is a compatibility summary containing the newest frame published by either source; )"
		R"(do not use it to pair both sources. totalMs and cpuTotalMs are live menu values that zero while idle. activeGpu and activeCpu describe the latest published frame for their own source. Each valid filtered capture cycle contributes exactly one history sample per matching known timer, )"
		R"(including zero when that timer did not run. )"
		R"(Same-name intervals are summed before one sample is pushed. acquiredSlots is the last completed GPU cycle's slot count; )"
		R"(peakAcquiredSlots is its session high-water mark. slotRefusals is cumulative, and any nonzero value means a GPU interval lacked a slot, although a requested CPU sample is still retained. Enable or disable capture via openshaders.profiler.")"
		R"(,"readOnly":true)"
		R"(,"inputSchema":{"type":"object"})"
		R"(})";
}
