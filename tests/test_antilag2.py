"""Execute the production Anti-Lag routing and driver-fallback paths with mocks."""

import unittest
from pathlib import Path

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


ROOT = Path(__file__).resolve().parents[1]


class AntiLag2Tests(unittest.TestCase):
    def test_rendered_fps_command_and_legacy_parameter(self):
        """Validate rendered limits directly while preserving the legacy parameter conversion."""
        source = (ROOT / "src/Features/Upscaling.cpp").read_text(encoding="utf-8")
        command = braced(source, "[](Feature* self, const json& args)")
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
using uint = unsigned;
namespace logger { void warn(const char*) {} }
struct Value {
    int64_t number; bool integer = true;
    bool is_number_integer() const { return integer; }
    template<class T> T get() const { return static_cast<T>(number); }
};
struct json {
    std::map<std::string, Value> values;
    bool contains(const char* key) const { return values.contains(key); }
    const Value& operator[](const char* key) const { return values.at(key); }
};
struct Feature {};
struct Upscaling : Feature { struct { unsigned antiLag2FPSLimit = 77; } settings; };
constexpr unsigned kGameplayMinRenderedFPSLimit = 20, kGameplayMaxRenderedFPSLimit = 240;
int main() {
    const auto command = PRODUCTION;
    for (const char* key : {"renderedFPS", "displayedFPS"})
        for (int64_t fps : {-1, 0, 19, 20, 21, 39, 40, 60, 120, 239, 240, 241, 479, 480, 481}) {
            Upscaling u;
            json args{{{key, Value{fps}}}};
            command(&u, args);
            const int64_t scale = std::string(key) == "displayedFPS" ? 2 : 1;
            const bool valid = fps >= 20 * scale && fps <= 240 * scale && fps % scale == 0;
            assert(u.settings.antiLag2FPSLimit == (valid ? unsigned(fps / scale) : 77u));
        }
    Upscaling u;
    command(&u, json{}); assert(u.settings.antiLag2FPSLimit == 77);
    command(&u, json{{{"renderedFPS", Value{120, false}}}}); assert(u.settings.antiLag2FPSLimit == 77);
    command(&u, json{{{"renderedFPS", Value{90}}, {"displayedFPS", Value{240}}}});
    assert(u.settings.antiLag2FPSLimit == 90);
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture.replace("PRODUCTION", command))

    def test_menu_frame_generation_guides(self):
        """Prepare menu guides once, preserving world inputs and checking resource readiness."""
        source = (ROOT / "src/Features/Upscaling.cpp").read_text(encoding="utf-8")
        body = braced(source, "void Upscaling::PrepareMenuFrameGeneration()")
        fixture = r'''
#include <cassert>
#define CS_GPU_PASS(name) (void)0
struct Texture { void* rtv = nullptr; };
struct Context {
    unsigned clears = 0; float depth = -1;
    void ClearRenderTargetView(void*, const float* value) {
        if (clears++ == 0) depth = value[0];
        else assert(value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0);
    }
};
namespace globals::d3d { Context* context; }
namespace globals::features {
struct { bool active = false; bool IsActive() { return active; } } reverseZ;
}
struct Upscaling {
    bool frameGenerationSceneVisited = false, frameGenerationPrepared = false;
    bool frameGenerationNeutralGuides = false, allowed = false, dlss = false;
    struct { Texture* depthBufferShared12 = nullptr; Texture* motionVectorBufferShared12 = nullptr; } dx12SwapChain;
    bool ShouldPrepareFrameGeneration() { return allowed; }
    bool UsesDLSSGFrameGen() { return dlss; }
    void PrepareMenuFrameGeneration();
};
PRODUCTION
int main() {
    for (unsigned flags = 0; flags != 512; ++flags) {
        Upscaling u; Context context;
        Texture depth, motion;
        globals::d3d::context = &context;
        globals::features::reverseZ.active = (flags & 256) != 0;
        u.frameGenerationSceneVisited = (flags & 1) != 0;
        u.frameGenerationPrepared = (flags & 2) != 0;
        u.allowed = (flags & 4) != 0; u.dlss = (flags & 8) != 0;
        u.dx12SwapChain.depthBufferShared12 = (flags & 16) ? &depth : nullptr;
        u.dx12SwapChain.motionVectorBufferShared12 = (flags & 32) ? &motion : nullptr;
        depth.rtv = (flags & 64) ? &depth : nullptr;
        motion.rtv = (flags & 128) ? &motion : nullptr;
        const bool expected = !(flags & 1) && !(flags & 2) && (flags & 4) && !(flags & 8) &&
            (flags & 16) && (flags & 32) && (flags & 64) && (flags & 128);
        u.PrepareMenuFrameGeneration();
        assert(context.clears == (expected ? 2u : 0u));
        assert(u.frameGenerationNeutralGuides == expected);
        assert(u.frameGenerationPrepared == (expected || bool(flags & 2)));
        if (expected) {
            assert(context.depth == ((flags & 256) ? 0.0f : 1.0f));
            u.PrepareMenuFrameGeneration();
            assert(context.clears == 2);
        }
    }
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture.replace("PRODUCTION", body))
        post = braced(source, "void Upscaling::PostDisplay()")
        self.assertLess(post.index("PrepareMenuFrameGeneration();"), post.index("SetUIBuffer();"))

    def test_input_limits_and_backend_routing(self):
        """Enforce identical caps across menus, loading, native D3D11, and active/inactive FG."""
        source = (ROOT / "src/Features/Upscaling.cpp").read_text()
        body = braced(source, "void Upscaling::OnBeforeInputPoll()")
        fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
namespace RE {
struct DialogueMenu { static constexpr int MENU_NAME = 1; };
struct LoadingMenu { static constexpr int MENU_NAME = 2; };
struct MainMenu { static constexpr int MENU_NAME = 4; };
}
struct UI {
    bool paused = false; int menus = 0;
    bool GameIsPaused() { return paused; }
    bool IsMenuOpen(int menu) { return (menus & menu) != 0; }
};
namespace globals::game { UI* ui = nullptr; }
namespace CS::AntiLag2 {
unsigned nativeCalls = 0, nativeLimit = 0;
struct InputPacing {
    int Apply(bool enabled, unsigned limit) {
        assert(enabled); ++nativeCalls; nativeLimit = limit; return 0;
    }
};
}
struct Upscaling {
    static constexpr unsigned kGameplayMinRenderedFPSLimit = 20;
    static constexpr unsigned kGameplayMaxRenderedFPSLimit = 240;
    static constexpr unsigned kAntiLag2RenderedFPSLimit = 60;
    bool d3d12SwapChainActive = true, frameGenerationActive = false;
    bool UsesDLSSGFrameGen() const { return d3d12SwapChainActive && dx12SwapChain.useDLSSG; }
    bool IsFrameGenerationActive() const {
        return d3d12SwapChainActive && settings.frameGenerationMode && frameGenerationActive;
    }
    struct { unsigned frameGenerationMode = 1, antiLag2FPSLimit = 60; } settings;
    struct {
        bool useDLSSG = false, backstop = false;
        unsigned calls = 0, limit = 0;
        void UpdateAntiLag2BeforeInput(unsigned fps, bool residual) {
            ++calls; limit = fps; backstop = residual;
        }
    } dx12SwapChain;
    void OnBeforeInputPoll();
};
PRODUCTION
int main() {
    UI ui;
    for (unsigned requested : {0u, 19u, 20u, 60u, 144u, 240u, 241u, UINT32_MAX})
        for (bool active : {false, true})
            for (bool dlss : {false, true})
                for (bool fg : {false, true})
                  for (bool acceptedFG : {false, true})
                    for (bool present : {false, true})
                        for (bool paused : {false, true})
                            for (int menus = 0; menus != 8; ++menus) {
                                Upscaling upscaling;
                                CS::AntiLag2::nativeCalls = 0;
                                upscaling.frameGenerationActive = acceptedFG;
                                ui.paused = paused; ui.menus = menus;
                                globals::game::ui = present ? &ui : nullptr;
                                upscaling.d3d12SwapChainActive = active;
                                upscaling.dx12SwapChain.useDLSSG = dlss;
                                upscaling.settings.frameGenerationMode = fg;
                                upscaling.settings.antiLag2FPSLimit = requested;
                                upscaling.OnBeforeInputPoll();
                                assert(upscaling.dx12SwapChain.calls == unsigned(active && !dlss));
                                assert(CS::AntiLag2::nativeCalls == unsigned(!active));
                                if (active && dlss) continue;
                                const unsigned rendered = std::clamp(requested, 20u, 240u);
                                const unsigned limit = rendered;
                                if (active) {
                                    assert(upscaling.dx12SwapChain.limit == limit);
                                    assert(upscaling.dx12SwapChain.backstop);
                                } else {
                                    assert(CS::AntiLag2::nativeLimit == limit);
                                }
                            }
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture.replace("PRODUCTION", body))

    def test_submission_and_shutdown_order(self):
        """Keep the end marker after submission and drain presents before context release."""
        source = (ROOT / "src/Features/Upscaling/DX12SwapChain.cpp").read_text()
        present = braced(source, "HRESULT DX12SwapChain::Present(")
        self.assertLess(present.index("PublishAntiLag2SwapChainData();"), present.index("upscaling.fidelityFX.Present("))
        self.assertLess(present.index("ExecuteCommandLists("), present.index("MarkAntiLag2EndOfFrame();"))
        self.assertLess(present.index("MarkAntiLag2EndOfFrame();"), present.index("swapChain->Present("))
        shutdown = braced(source, "DX12SwapChain::~DX12SwapChain()")
        self.assertLess(shutdown.index("ffx::Dispatch("), shutdown.index("PublishAntiLag2SwapChainData();"))
        self.assertLess(shutdown.index("PublishAntiLag2SwapChainData();"), shutdown.index("DeInitialize("))

    def test_driver_failure_and_residual_timer(self):
        """Run successful, unsupported, and failing driver updates with timer ownership checks."""
        source = (ROOT / "src/Features/Upscaling/DX12SwapChain.cpp").read_text()
        body = braced(source, "void DX12SwapChain::UpdateAntiLag2BeforeInput(")
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <utility>
using HRESULT = int;
constexpr HRESULT E_NOINTERFACE = -1;
bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
bool FAILED(HRESULT hr) { return hr < 0; }
namespace logger {
unsigned warnings = 0;
template<class... Args> void warn(const char*, Args...) { ++warnings; }
template<class... Args> void info(const char*, Args...) {}
}
namespace AMD::AntiLag2DX12 {
int result = 0; unsigned calls = 0;
HRESULT Update(int*, bool enabled, unsigned fps) {
    assert(enabled && fps == 60); ++calls; return result;
}
}
struct DX12SwapChain {
    bool antiLag2Available = false, antiLag2Enabled = false, antiLag2UpdateFailureLogged = false;
    uint32_t antiLag2LastFPSLimit = UINT32_MAX;
    int antiLag2Context = 0;
    struct {
        unsigned calls = 0; bool enabled = false;
        int Apply(bool active, unsigned fps) { assert(fps == 60); ++calls; enabled = active; return 0; }
    } antiLag2InputPacing;
    void UpdateAntiLag2BeforeInput(uint32_t fpsLimit, bool timerBackstop);
};
PRODUCTION
int main() {
    for (bool available : {false, true})
        for (int result : {-1, 0, 1})
            for (bool backstop : {false, true}) {
                DX12SwapChain chain;
                chain.antiLag2Available = available;
                AMD::AntiLag2DX12::result = result;
                AMD::AntiLag2DX12::calls = 0; logger::warnings = 0;
                for (int i = 0; i != 3; ++i) chain.UpdateAntiLag2BeforeInput(60, backstop);
                assert(AMD::AntiLag2DX12::calls == (available ? 3u : 0u));
                assert(chain.antiLag2Enabled == (available && result >= 0));
                assert(chain.antiLag2InputPacing.calls == 3);
                assert(chain.antiLag2InputPacing.enabled == (backstop || !chain.antiLag2Enabled));
                assert(logger::warnings == unsigned(available && result < 0));
            }
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture.replace("PRODUCTION", body))


if __name__ == "__main__":
    unittest.main()
