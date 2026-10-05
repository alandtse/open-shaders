import re
import unittest

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import ROOT, braced


CPP = (ROOT / "src/Features/Skylighting.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/Features/Skylighting.h").read_text(encoding="utf-8")


def scheduling_fixture():
    prepare = CPP[CPP.index("\tconst uint sliceCount = settings.EnableIncrementalProbeUpdates"):
                  CPP.index("\n\treturn {", CPP.index("\tconst uint sliceCount = settings.EnableIncrementalProbeUpdates"))]
    advance = CPP[CPP.index("\t\t\tlastProbeUpdateCapture = frameCount;"):
                  CPP.index("\n\t\t\tpreviousProbeCell = pendingProbeCell;")]
    reset = braced(CPP, "void Skylighting::ClearProbes()")
    reset = reset[reset.index("\tprobeDataReady = false;"):reset.rfind("}")]
    warmup = re.search(r"static constexpr uint probeHistoryWarmupFrames = \d+;", HEADER).group()
    return r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
using uint = unsigned int;
struct float3 { float x = 0, y = 0, z = 0; };
namespace globals {
struct State { uint frameCount = 0; } stateStorage;
auto* state = &stateStorage;
}
namespace Util {
template<class T> T ClampFinite(T value, T low, T high, T fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
}
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct Skylighting {
    SETTINGS
    WARMUP
    uint probeArrayDims[3] = {256, 256, 128};
    uint sliceCursor = 0, sliceCaptureMask = 0, activeSliceCount = 0;
    uint forcedFullUpdateFrames = probeHistoryWarmupFrames;
    uint dispatchSliceStart = 0, dispatchSliceCount = 0;
    uint frameCount = 0, lastProbeUpdateFrame = ~0u, lastProbeUpdateCapture = ~0u;
    uint lastOcclusionRenderFrame = ~0u, occlusionCaptureCorner = 0, nextOcclusionCorner = 0;
    bool probeDataReady = true, queuedResetSkylighting = false;
    void ResetSkylighting() { queuedResetSkylighting = true; }
    void GameLoaded();
    void OnSceneTransitionReset(bool);
    void LoadSettings(Settings&);
    void ClearProbes() { RESET }
    void Prepare(bool moved = false) { float3 cellIDDiff{moved ? 1.f : 0.f, 0, 0}; PREPARE }
    void Advance(uint corner) { frameCount = occlusionCaptureCorner = corner; ADVANCE }
};
using json = Skylighting::Settings;
LOAD
'''.replace("SETTINGS", braced(HEADER, "struct Settings") + " settings;").replace(
        "WARMUP", warmup).replace("RESET", reset).replace("PREPARE", prepare).replace(
        "ADVANCE", advance).replace("LOAD", "\n".join(braced(CPP, declaration) for declaration in (
            "void Skylighting::LoadSettings(", "void Skylighting::GameLoaded(",
            "void Skylighting::OnSceneTransitionReset(")))


class SkylightingSchedulingTests(unittest.TestCase):
    compile_and_run = runtime.SceneSettingsRuntimeTests.compile_and_run

    def test_warmup_covers_shader_confidence_fade(self):
        shader = (ROOT / "features/Skylighting/Shaders/Skylighting/UpdateProbesCS.hlsl").read_text(encoding="utf-8")
        samples = int(re.search(r"fadeInThreshold = (\d+)", shader).group(1))
        frames = int(re.search(r"probeHistoryWarmupFrames = (\d+)", HEADER).group(1))
        self.assertGreaterEqual(frames, 4 * samples)
        self.assertEqual(frames % 4, 0)

    def test_stationary_recovery_and_schedule_changes(self):
        self.compile_and_run(scheduling_fixture() + r'''
int main() {
    for (uint depth : {64u, 96u, 128u}) {
        for (uint slices : {1u, 13u, 16u, 128u}) {
            Skylighting s;
            s.probeArrayDims[2] = depth;
            s.settings.EnableIncrementalProbeUpdates = true;
            s.settings.StableSliceCount = slices;
            s.ClearProbes();
            s.Prepare();
            for (uint i = 0; i < 8; ++i) s.Advance(0);
            check(s.forcedFullUpdateFrames == Skylighting::probeHistoryWarmupFrames,
                  "Repeated quadrants must not exhaust reset recovery");
            s.Prepare(true);
            check(s.forcedFullUpdateFrames == Skylighting::probeHistoryWarmupFrames,
                  "Movement must not shorten reset recovery");
            for (uint i = 0; i < Skylighting::probeHistoryWarmupFrames; ++i) {
                s.Prepare();
                check(s.dispatchSliceStart == 0 && s.dispatchSliceCount == depth,
                      "Stationary recovery must dispatch the entire grid");
                s.Advance(i % 4);
            }
            check(s.forcedFullUpdateFrames == 0, "Stationary recovery must finish");
            std::array<uint, 128> quadrants{};
            const uint count = std::min(slices, depth);
            for (uint i = 0; i < 4 * ((depth + count - 1) / count); ++i) {
                s.Prepare();
                check(s.dispatchSliceCount > 0 && s.dispatchSliceStart + s.dispatchSliceCount <= depth,
                      "Tail batches must stay inside every supported grid");
                for (uint z = s.dispatchSliceStart; z < s.dispatchSliceStart + s.dispatchSliceCount; ++z)
                    quadrants[z] |= 1u << (i % 4);
                s.Advance(i % 4);
            }
            for (uint z = 0; z < depth; ++z)
                check(quadrants[z] == 15, "Stationary slices must receive all capture quadrants");
            s.Prepare(true);
            check(s.forcedFullUpdateFrames == 4, "Ordinary movement retains its shorter warmup");
        }
    }
    Skylighting s;
    s.forcedFullUpdateFrames = 0;
    for (bool enabled : {true, false, true}) {
        auto selected = s.settings;
        selected.EnableIncrementalProbeUpdates = enabled;
        selected.StableSliceCount = 13;
        s.LoadSettings(selected);
        check(!s.queuedResetSkylighting && s.probeDataReady,
              "Scheduling-only settings must preserve valid lighting history");
        s.Prepare();
        check(s.forcedFullUpdateFrames == 0, "Scheduling edits must not trigger a rebuild");
    }
    s.Advance(0);
    s.Advance(1);
    s.settings.StableSliceCount = 32;
    s.Prepare();
    check(s.sliceCursor == 0 && s.sliceCaptureMask == 0,
          "An enlarged batch must not inherit partial quadrant coverage");
    s.Advance(2);
    s.Advance(3);
    check(s.sliceCursor == 0, "Every quadrant is required for the new batch");
    s.Advance(0);
    s.Advance(1);
    check(s.sliceCursor == 32, "The new batch can advance after complete coverage");
    s.ClearProbes();
    check(!s.probeDataReady && s.forcedFullUpdateFrames == Skylighting::probeHistoryWarmupFrames,
          "Every travel reset must restart full recovery without movement");
}
''')

    def test_interrupted_recovery_and_loading_callbacks(self):
        self.compile_and_run(scheduling_fixture() + r'''
int main() {
    Skylighting s;
    s.settings.EnableIncrementalProbeUpdates = true;
    s.ClearProbes();
    for (uint i = 0; i < 8; ++i) { s.Prepare(); s.Advance(i % 4); }
    const uint remaining = s.forcedFullUpdateFrames;
    check(remaining > 4 && remaining < Skylighting::probeHistoryWarmupFrames,
          "The fixture must start inside an unfinished reset warmup");
    for (bool enabled : {false, true}) {
        auto selected = s.settings;
        selected.EnableIncrementalProbeUpdates = enabled;
        selected.StableSliceCount = 0;
        s.LoadSettings(selected);
        check(s.settings.StableSliceCount == 1 && !s.queuedResetSkylighting,
              "Scheduling edits must validate bounds without clearing history");
        for (uint i = 0; i < 20; ++i) s.Prepare(i == 0);
        check(s.forcedFullUpdateFrames == remaining,
              "Settings edits, movement and missed dispatches must preserve reset work");
    }
    for (uint i = 0; i < remaining; ++i) {
        s.Prepare();
        check(s.dispatchSliceStart == 0 && s.dispatchSliceCount == 128,
              "Recovery must stay full-grid after mode changes");
        s.Advance(i % 4);
    }
    s.Prepare();
    check(s.forcedFullUpdateFrames == 0 && s.dispatchSliceCount == 1,
          "Interrupted recovery must resume the newly selected schedule");
    for (uint event = 0; event < 3; ++event) {
        s.queuedResetSkylighting = false;
        if (event == 0) s.GameLoaded();
        else s.OnSceneTransitionReset(event == 1);
        check(s.queuedResetSkylighting,
              "Save load and both loading-menu transitions must request recovery");
        s.ClearProbes();
        check(!s.probeDataReady && s.forcedFullUpdateFrames == Skylighting::probeHistoryWarmupFrames,
              "Each transition must restart complete stationary recovery");
    }
}
''')


if __name__ == "__main__":
    unittest.main()
