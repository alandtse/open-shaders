import unittest

import test_scene_settings_runtime as runtime
from test_skylighting_scheduling import CPP, scheduling_fixture


class SkylightingCadenceTests(unittest.TestCase):
    compile_and_run = runtime.SceneSettingsRuntimeTests.compile_and_run

    def test_repeated_travel_bypasses_cadence_and_edits_preserve_history(self):
        capture = CPP[CPP.index("\tif (settings.EnableReducedUpdateFrequency && forcedFullUpdateFrames"):
                      CPP.index('\n\tCS_GPU_PASS("Skylighting::SkylightingMask")')]
        due = CPP[CPP.index("\tconst uint probeInterval = std::max"):
                  CPP.index("\n\t\tCS_GPU_PASS_SELECT", CPP.index("\tconst uint probeInterval = std::max"))]
        due = due[:due.rfind(" {")].replace("\tif (updateShader", "\treturn (updateShader") + ";"
        fixture = scheduling_fixture().replace(
            "void Advance(uint corner) { frameCount = occlusionCaptureCorner = corner;",
            "void CommitUpdate() {").replace(
            "void LoadSettings(Settings&);", r'''
    void LoadSettings(Settings&);
    bool Capture(bool cellMoved = false) {
        if (lastOcclusionRenderFrame == globals::state->frameCount) return false;
        CAPTURE
        occlusionCaptureCorner = nextOcclusionCorner;
        ++frameCount;
        lastOcclusionRenderFrame = globals::state->frameCount;
        return true;
    }
    bool Due(bool updateShader = true) {
        bool comparisonSampler = true;
        DUE
    }
'''.replace("CAPTURE", capture.replace("return;", "return false;")).replace("DUE", due))
        self.compile_and_run(fixture + r'''
int main() {
    for (uint depth : {64u, 96u, 128u}) {
        for (bool incremental : {false, true}) {
            for (uint capture = 1; capture <= 32; ++capture) {
                for (uint probe = capture; probe <= 32; ++probe) {
                    Skylighting s;
                    s.probeArrayDims[2] = depth;
                    s.settings.EnableIncrementalProbeUpdates = incremental;
                    s.settings.StableSliceCount = 13;
                    s.settings.EnableReducedUpdateFrequency = true;
                    s.settings.OcclusionUpdateInterval = capture;
                    s.settings.ProbeUpdateInterval = probe;
                    for (uint travel = 0; travel < 2; ++travel) {
                        s.ClearProbes();
                        for (uint i = 0; i < Skylighting::probeHistoryWarmupFrames; ++i) {
                            ++globals::state->frameCount;
                            s.Prepare();
                            check(s.Capture(), "Every travel recovery must bypass capture delays");
                            check(!s.Capture(), "A frame must not capture twice");
                            check(!s.Due(false), "Unavailable shaders must not consume warmup");
                            check(s.Due(), "Probe cadence must not delay travel recovery");
                            check(s.dispatchSliceStart == 0 && s.dispatchSliceCount == depth,
                                  "Travel recovery must update the full grid while stationary");
                            s.CommitUpdate();
                            check(!s.Due(), "A capture must not be consumed twice");
                        }
                        check(s.forcedFullUpdateFrames == 0, "Recovery must finish without movement");
                        std::array<uint, 128> quadrants{};
                        uint captures = 0;
                        for (uint i = 0; i < 4 * ((depth + 12) / 13) * probe; ++i) {
                            ++globals::state->frameCount;
                            s.Prepare();
                            captures += s.Capture();
                            if (!s.Due()) continue;
                            for (uint z = s.dispatchSliceStart; z < s.dispatchSliceStart + s.dispatchSliceCount; ++z)
                                quadrants[z] |= 1u << s.occlusionCaptureCorner;
                            s.CommitUpdate();
                        }
                        for (uint z = 0; z < depth; ++z)
                            check(quadrants[z] == 15, "Cadence must not starve stationary quadrants");
                        check(captures <= 4 * ((depth + 12) / 13) * probe / capture,
                              "Configured capture savings must resume after recovery");
                    }
                    s.probeDataReady = true;
                    s.sliceCaptureMask = 3;
                    for (bool reduced : {false, true}) {
                        auto selected = s.settings;
                        selected.EnableReducedUpdateFrequency = reduced;
                        selected.OcclusionUpdateInterval = 32;
                        selected.ProbeUpdateInterval = 1;
                        s.LoadSettings(selected);
                        check(!s.queuedResetSkylighting && s.probeDataReady && s.sliceCaptureMask == 3,
                              "Cadence edits must preserve valid history and quadrant coverage");
                        check(s.settings.ProbeUpdateInterval == 32,
                              "Scheduling-only edits still validate interval bounds");
                    }
                }
            }
        }
    }
    for (uint firstFrame : {0u, ~0u - 40u}) {
        Skylighting s;
        s.settings.EnableReducedUpdateFrequency = true;
        s.settings.EnableIncrementalProbeUpdates = true;
        s.settings.OcclusionUpdateInterval = s.settings.ProbeUpdateInterval = 32;
        s.ClearProbes();
        globals::state->frameCount = firstFrame;
        for (uint i = 0; i < 2; ++i) {
            ++globals::state->frameCount;
            s.Prepare();
            check(s.Capture() && s.Due(), "Recovery must begin without movement");
            s.CommitUpdate();
        }
        const uint remaining = s.forcedFullUpdateFrames;
        const uint consumedQuadrants = s.sliceCaptureMask;
        const uint pendingQuadrant = s.nextOcclusionCorner;
        for (uint i = 0; i < 7; ++i) {
            ++globals::state->frameCount;
            s.Prepare();
            check(s.Capture() && !s.Due(false), "The fixture must interrupt probe consumption");
            check(s.forcedFullUpdateFrames == remaining && s.sliceCaptureMask == consumedQuadrants &&
                      s.occlusionCaptureCorner == pendingQuadrant,
                  "Missed updates must preserve warmup and retry the pending quadrant");
        }
        for (uint i = 2; i < Skylighting::probeHistoryWarmupFrames; ++i) {
            ++globals::state->frameCount;
            s.Prepare();
            check(s.Capture() && s.Due(), "Recovery must resume across frame-counter wrap");
            s.CommitUpdate();
        }
        check(s.forcedFullUpdateFrames == 0, "Interrupted warmup must finish after consuming all quadrants");
        ++globals::state->frameCount;
        s.Prepare();
        check(!s.Capture() && !s.Due(), "Capture throttling must resume immediately after recovery");
        s.settings.EnableIncrementalProbeUpdates = false;
        ++globals::state->frameCount;
        check(s.Capture(true), "Movement must bypass capture throttling");
        s.Prepare(true);
        check(s.Due() && s.dispatchSliceStart == 0 && s.dispatchSliceCount == 128,
              "Movement must bypass full-grid probe throttling without discarding history");
    }
}
''')


if __name__ == "__main__":
    unittest.main()
