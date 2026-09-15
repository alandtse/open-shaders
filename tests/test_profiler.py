import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime


PRELUDE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ID3D11Device {};
struct ID3D11DeviceContext {};
struct LARGE_INTEGER { int64_t QuadPart = 0; };
uint64_t clockTicks = 0, cpuReads = 0, gpuWrites = 0, gpuBatches = 0;
bool QueryPerformanceFrequency(LARGE_INTEGER* value) { value->QuadPart = 1000; return true; }
bool QueryPerformanceCounter(LARGE_INTEGER* value) { ++cpuReads; value->QuadPart = ++clockTicks; return true; }

namespace Util {
class TimestampQueryBatch {
    struct Interval { uint64_t begin = 0, end = 0; bool closed = false; };
    uint32_t capacity = 0;
    std::vector<Interval> intervals;
public:
    enum class Status { NotReady, Disjoint, Ok };
    static inline Status status = Status::Ok;
    void Configure(uint32_t count, const char*) { capacity = count; }
    bool Preallocate(ID3D11Device*) { return true; }
    void ReleaseQueries() { intervals.clear(); }
    void Reset() { intervals.clear(); }
    bool BeginBatch(ID3D11Device*, ID3D11DeviceContext*) { ++gpuBatches; return true; }
    void EndBatch(ID3D11DeviceContext*) {}
    int AcquireInterval(ID3D11Device*, ID3D11DeviceContext*) {
        if (intervals.size() >= capacity) return -1;
        intervals.push_back({ ++clockTicks, 0, false });
        ++gpuWrites;
        return static_cast<int>(intervals.size() - 1);
    }
    void CloseInterval(ID3D11DeviceContext*, uint32_t index) {
        auto& interval = intervals.at(index);
        interval.end = ++clockTicks;
        interval.closed = true;
        ++gpuWrites;
    }
    Status TryResolve(ID3D11DeviceContext*, const std::function<void(uint32_t, uint64_t, uint64_t)>& visit) const {
        if (status != Status::Ok) return status;
        for (uint32_t index = 0; index < intervals.size(); ++index)
            if (intervals[index].closed)
                visit(index, intervals[index].end - intervals[index].begin, 1000);
        return Status::Ok;
    }
};
}
'''

HELPERS = r'''
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
void pass(Profiler& profiler, std::string_view name) {
    if (profiler.BeginPass(name, false)) profiler.EndPass(false);
}
const Profiler::TimerResult* find(const Profiler& profiler, std::string_view name) {
    for (const auto& result : profiler.GetResults())
        if (result.name == name) return &result;
    return nullptr;
}
float topLevelTotal(const Profiler& profiler) {
    float total = 0;
    for (const auto& result : profiler.GetResults()) total += result.topLevelMs;
    return total;
}
using Mode = Profiler::CaptureMode;
namespace globals { Profiler* profiler = nullptr; }
'''


class ProfilerTests(unittest.TestCase):
    def compile_profiler(self, body, extra=""):
        header = (ROOT / "src/Profiler.h").read_text(encoding="utf-8")
        header = "\n".join(line for line in header.splitlines()
                           if not line.startswith(("#include", "#pragma")))
        implementation = (ROOT / "src/Profiler.cpp").read_text(encoding="utf-8")
        implementation = implementation.replace('#include "Profiler.h"', "")
        source = PRELUDE + header + implementation + HELPERS + extra
        source += "\nint main() { ID3D11Device device; ID3D11DeviceContext context;\n" + body + "\n}"
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)

    def test_capture_modes_filters_and_pending_gpu(self):
        self.compile_profiler(r'''
Profiler profiler;
profiler.Initialize(&device, &context);
pass(profiler, "Idle::Pass");
check(cpuReads == 0 && gpuWrites == 0 && gpuBatches == 0, "Idle passes acquire no timing");
profiler.RequestCapture(Mode::CPU, "First::");
profiler.EndFrame(0);
pass(profiler, "Filtered::Pass");
check(cpuReads == 0, "Filtered CPU passes do not read the clock");
pass(profiler, "First::Pass");
profiler.RequestCapture(Mode::GPU, "First::");
profiler.EndFrame(1);
check(cpuReads == 2 && gpuWrites == 0 && gpuBatches == 0, "CPU mode issues no GPU queries");
check(profiler.GetCapturedCpuFrameCount() == 1, "CPU samples publish at their frame boundary");
pass(profiler, "First::Pass");
check(cpuReads == 2 && gpuWrites == 2, "GPU mode does not read CPU scope clocks");
profiler.RequestCapture(Mode::GPU, "First::");
profiler.RequestCapture(Mode::CPU, "Second::");
profiler.EndFrame(2);
Util::TimestampQueryBatch::status = Util::TimestampQueryBatch::Status::NotReady;
for (uint32_t frame = 3; frame <= 10; ++frame) {
    pass(profiler, "First::Pass");
    pass(profiler, "Second::Pass");
    profiler.RequestCapture();
    profiler.EndFrame(frame);
}
check(profiler.GetCapturedCpuFrameCount() == 10, "CPU publication continues while GPU data is pending");
check(find(profiler, "First::Pass")->activeCpu && find(profiler, "Second::Pass")->activeCpu,
    "Different request filters broaden capture and modes combine");
check(profiler.GetCapturedGpuFrameCount() == 0, "Pending GPU data is not published as a zero");
''')

    def test_partial_ring_drains_after_mode_switch(self):
        self.compile_profiler(r'''
for (uint32_t capturedFrames : { 1u, 2u }) {
    for (int nextMode = 0; nextMode < 3; ++nextMode) {
        Profiler profiler;
        profiler.Initialize(&device, &context);
        profiler.RequestCapture(Mode::GPU);
        profiler.EndFrame(0);
        for (uint32_t frame = 1; frame <= capturedFrames; ++frame) {
            pass(profiler, "GPU::Pass");
            profiler.RequestCapture(frame < capturedFrames ? Mode::GPU : (nextMode == 0 ? Mode::CPU : Mode::None));
            profiler.EndFrame(frame);
        }
        if (nextMode == 2) profiler.SetUserEnabled(false);
        const auto previousGpuWrites = gpuWrites;
        const auto previousGpuBatches = gpuBatches;
        for (uint32_t frame = capturedFrames + 1; frame <= 12; ++frame) {
            pass(profiler, "CPU::Pass");
            if (nextMode == 0) profiler.RequestCapture(Mode::CPU);
            profiler.EndFrame(frame);
        }
        check(profiler.GetCapturedGpuFrameCount() == capturedFrames, "Partial GPU rings drain during CPU, idle, and disabled frames");
        check(profiler.GetResolvedTotalTimeMs() == topLevelTotal(profiler), "Draining preserves resolved accounting");
        check(gpuWrites == previousGpuWrites && gpuBatches == previousGpuBatches, "Draining does not acquire new GPU queries");
        check(profiler.GetTotalTimeMs() == 0, "Inactive GPU acquisition shows a zero live total");
        profiler.SetUserEnabled(true);
        profiler.RequestCapture(Mode::GPU);
        profiler.EndFrame(13);
        pass(profiler, "Resumed::Pass");
        profiler.EndFrame(14);
        for (uint32_t frame = 15; frame <= 20; ++frame) profiler.EndFrame(frame);
        check(profiler.GetCapturedGpuFrameCount() == 14 && find(profiler, "Resumed::Pass")->activeGpu,
            "Resuming capture does not overwrite or replay an older batch");
    }
}
''')

    def test_retirement_preserves_each_published_source(self):
        self.compile_profiler(r'''
for (bool startOnGpu : { false, true }) {
    Profiler profiler;
    profiler.Initialize(&device, &context);
    const Mode firstMode = startOnGpu ? Mode::GPU : Mode::CPU;
    const Mode secondMode = startOnGpu ? Mode::CPU : Mode::GPU;
    profiler.RequestCapture(firstMode, "First::");
    profiler.EndFrame(0);
    for (uint32_t frame = 1; frame <= 4; ++frame) {
        pass(profiler, "First::Pass");
        profiler.RequestCapture(frame == 4 ? secondMode : firstMode, frame == 4 ? "Second::" : "First::");
        profiler.EndFrame(frame);
    }
    for (uint32_t frame = 5; frame <= 75; ++frame) {
        pass(profiler, "Second::Pass");
        profiler.RequestCapture(secondMode, "Second::");
        profiler.EndFrame(frame);
    }
    const auto* first = find(profiler, "First::Pass");
    check(first && (startOnGpu ? first->activeGpu : first->activeCpu), "Other-source activity cannot retire the published snapshot");
    check(profiler.GetResolvedTotalTimeMs() == topLevelTotal(profiler), "Published GPU total retains its rows");
    if (!startOnGpu) check(profiler.GetResolvedCpuTotalTimeMs() == first->cpuTimeMs, "Published CPU total retains its rows");
    profiler.RequestCapture();
    profiler.EndFrame(76);
    for (uint32_t frame = 77; frame <= 145; ++frame) {
        pass(profiler, "Second::Pass");
        profiler.RequestCapture();
        profiler.EndFrame(frame);
    }
    check(!find(profiler, "First::Pass"), "Inactive timers still retire after both snapshots advance");
}
''')

    def test_nested_repeated_empty_and_capacity_cycles(self):
        self.compile_profiler(r'''
Profiler profiler;
profiler.Initialize(&device, &context);
profiler.RequestCapture();
profiler.EndFrame(0);
for (uint32_t frame = 1; frame <= 4; ++frame) {
    check(profiler.BeginPass("Parent::Pass", false), "Parent opens");
    pass(profiler, "Child::Pass");
    pass(profiler, "Child::Pass");
    profiler.EndPass(false);
    profiler.RequestCapture();
    profiler.EndFrame(frame);
}
const auto* child = find(profiler, "Child::Pass");
check(child && child->historyCount == 1 && child->cpuHistoryCount == 4, "Repeated names publish once per source cycle");
check(topLevelTotal(profiler) == profiler.GetResolvedTotalTimeMs(), "Nested totals do not double-count children");
float selfTotal = 0;
for (const auto& result : profiler.GetResults()) selfTotal += result.gpuTimeMs;
check(selfTotal == topLevelTotal(profiler), "GPU self times sum to the inclusive parent duration");
for (uint32_t index = 0; index < Profiler::kMaxTimers; ++index) pass(profiler, "Capacity::Pass");
pass(profiler, "Overflow::Pass");
profiler.RequestCapture(Mode::GPU, "Missing::");
profiler.EndFrame(5);
check(profiler.GetSlotRefusals() == 1 && find(profiler, "Overflow::Pass")->activeCpu, "CPU samples survive GPU capacity refusal");
for (uint32_t frame = 6; frame <= 12; ++frame) {
    pass(profiler, "Filtered::Pass");
    profiler.RequestCapture(Mode::GPU, "Missing::");
    profiler.EndFrame(frame);
}
check(profiler.GetAcquiredSlots() == 0 && profiler.GetResolvedTotalTimeMs() == 0, "Empty filtered cycles never reuse old interval metadata");
profiler.ClearTimers();
for (uint32_t frame = 13; frame <= 17; ++frame) profiler.EndFrame(frame);
check(profiler.GetResults().empty(), "Cleared queued intervals cannot resurrect timers");
profiler.Initialize(&device, &context);
check(profiler.GetResults().empty() && profiler.GetCapturedCpuFrameCount() == 0 && profiler.GetCapturedGpuFrameCount() == 0,
    "Reinitialization resets both publications");
''')

    def test_statistics_group_percentiles_use_frame_sums(self):
        renderer = (ROOT / "src/Menu/ProfilingRenderer.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/Menu/ProfilingRenderer.h").read_text(encoding="utf-8")
        start = renderer.index("// Rebuilds a TimerResult")
        end = renderer.index("void ProfilingRenderer::UpdateStatistics", start)
        extra = "\nstatic constexpr float kMaxDisplayTimingSampleMs = 1000.0f;\n"
        extra += runtime.braced(renderer, "static bool IsDisplayTimingSampleValid")
        extra += runtime.braced(renderer, "static bool HasLiveTimingMode")
        extra += renderer[start:end]
        extra += "\nclass ProfilingRenderer { public:\n"
        extra += runtime.braced(header, "struct PassEntry") + ";\n"
        extra += runtime.braced(header, "struct GroupEntry") + ";\n"
        extra += r'''
static inline std::vector<GroupEntry> cachedGroups;
static inline float cachedTotalAvgMs = 0, cachedMaxAvgMs = 0, cachedMaxP95Ms = 0, cachedMaxP99Ms = 0;
static void UpdateStatistics(bool cpuMode);
};
'''
        extra += runtime.braced(renderer, "void ProfilingRenderer::UpdateStatistics")
        self.compile_profiler(r'''
for (bool cpuMode : { false, true }) {
    Profiler profiler;
    globals::profiler = &profiler;
    profiler.Initialize(&device, &context);
    const auto mode = cpuMode ? Mode::CPU : Mode::GPU;
    profiler.RequestCapture(mode);
    profiler.EndFrame(0);
    for (uint32_t frame = 1; frame <= 120; ++frame) {
        const uint64_t spike = frame <= 60 ? 10 : 0;
        check(profiler.BeginPass("Feature::First", false), "First statistics pass opens");
        if (frame % 2) clockTicks += spike;
        profiler.EndPass(false);
        check(profiler.BeginPass("Feature::Second", false), "Second statistics pass opens");
        if (!(frame % 2)) clockTicks += spike;
        profiler.EndPass(false);
        if (frame < 120) profiler.RequestCapture(mode);
        profiler.EndFrame(frame);
    }
    for (uint32_t frame = 121; frame <= 126; ++frame) profiler.EndFrame(frame);
    ProfilingRenderer::UpdateStatistics(cpuMode);
    check(ProfilingRenderer::cachedGroups.size() == 1, "Statistics groups feature passes together");
    const auto& group = ProfilingRenderer::cachedGroups.front();
    check(group.passes.size() == 2, "Both individual pass rows remain visible");
    check(group.totalAvgMs == 7, "The full statistics table keeps its longer history window");
    check(group.totalP95Ms == 12 && group.totalP99Ms == 12, "Group percentiles use summed frames, not summed percentiles");
    check(group.passes[0].p95Ms == 11 && group.passes[1].p95Ms == 11, "Individual pass percentiles remain independent");
}
''', extra)

    def test_overlay_excludes_engine_self_time(self):
        overlay = (ROOT / "src/Features/PerformanceOverlay.cpp").read_text(encoding="utf-8")
        begin = overlay.index("float csPassesTime = ")
        end = overlay.index("float csPercent = ", begin)
        extra = "\nfloat overlayTime() {\n" + overlay[begin:end] + "\nreturn csPassesTime;\n}\n"
        self.compile_profiler(r'''
Profiler profiler;
globals::profiler = &profiler;
profiler.Initialize(&device, &context);
profiler.RequestCapture(Mode::GPU);
profiler.EndFrame(0);
for (uint32_t frame = 1; frame <= 4; ++frame) {
    check(profiler.BeginPass("Effects", false), "Engine scope opens");
    pass(profiler, "Feature::Compute");
    profiler.EndPass(false);
    pass(profiler, "OtherFeature::Compute");
    profiler.RequestCapture(Mode::GPU);
    profiler.EndFrame(frame);
}
const float expected = find(profiler, "Feature::Compute")->gpuTimeMs + find(profiler, "OtherFeature::Compute")->gpuTimeMs;
check(overlayTime() == expected, "OS Passes excludes engine self time but keeps nested and independent feature passes");
profiler.EndFrame(5);
for (uint32_t frame = 6; frame <= 10; ++frame) profiler.EndFrame(frame);
check(overlayTime() == 0, "An idle overlay does not replay the retained GPU snapshot");
''', extra)


if __name__ == "__main__":
    unittest.main()
