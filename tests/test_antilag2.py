"""Execute the production Anti-Lag routing and driver-fallback paths with mocks."""

import unittest
from pathlib import Path

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


ROOT = Path(__file__).resolve().parents[1]


class AntiLag2Tests(unittest.TestCase):
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
                                const unsigned limit = rendered * (active && fg && acceptedFG ? 1u : 2u);
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
    void UpdateAntiLag2BeforeInput(uint32_t fpsLimit, bool sceneBackstop);
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
