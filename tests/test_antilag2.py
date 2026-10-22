"""Execute the production Anti-Lag routing and driver-fallback paths with mocks."""

import os
import unittest
from pathlib import Path

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


ROOT = Path(__file__).resolve().parents[1]


class AntiLag2Tests(unittest.TestCase):
    def test_live_control_command(self):
        """Apply independent boolean controls without partially accepting malformed commands."""
        source = (ROOT / "src/Features/Upscaling.cpp").read_text(encoding="utf-8")
        command = braced(source[source.index('FEATURE_COMMAND("setAntiLag2",'):], "[](Feature* self, const json& args)")
        fixture = r'''
#include <cassert>
#include <map>
#include <string>
namespace logger { void warn(const char*) {} }
struct Value {
    bool value, boolean = true;
    bool is_boolean() const { return boolean; }
    template<class T> T get() const { return static_cast<T>(value); }
};
struct json {
    std::map<std::string, Value> values;
    bool contains(const char* key) const { return values.contains(key); }
    const Value& operator[](const char* key) const { return values.at(key); }
};
struct Feature {};
struct Upscaling : Feature {
    struct {
        bool antiLag2Enabled = true, antiLag2FPSLimiterEnabled = true, antiLag2FPSLimiterGameplayOnly = false;
    } settings;
};
int main() {
    const auto command = PRODUCTION;
    for (unsigned bits = 0; bits != 8; ++bits) {
        Upscaling u;
        command(&u, json{{{"enabled", Value{bool(bits & 1)}},
            {"fpsLimiterEnabled", Value{bool(bits & 2)}}, {"gameplayOnly", Value{bool(bits & 4)}}}});
        assert(u.settings.antiLag2Enabled == bool(bits & 1));
        assert(u.settings.antiLag2FPSLimiterEnabled == bool(bits & 2));
        assert(u.settings.antiLag2FPSLimiterGameplayOnly == bool(bits & 4));
    }
    Upscaling u;
    command(&u, json{{{"enabled", Value{false}}, {"fpsLimiterEnabled", Value{false, false}}}});
    assert(u.settings.antiLag2Enabled && u.settings.antiLag2FPSLimiterEnabled);
    command(&u, json{{{"gameplayOnly", Value{true}}}});
    assert(u.settings.antiLag2Enabled && u.settings.antiLag2FPSLimiterEnabled && u.settings.antiLag2FPSLimiterGameplayOnly);
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture.replace("PRODUCTION", command))

    @unittest.skipUnless(os.name == "nt", "Requires Windows SDK headers")
    def test_native_timer_disable_and_reenable(self):
        """Reset the actual timer's deadline when disabled so re-enabling starts fresh."""
        header = (ROOT / "src/Features/Upscaling/antilag2inputpacing.h").as_posix()
        fixture = '#include "' + header + '"\n' + r'''
#include <cassert>
int main() {
    CS::AntiLag2::InputPacing pacing;
    for (unsigned fps : {20u, 60u, 240u}) {
        assert(pacing.Apply(false, 0).count() == 0);
        assert(pacing.Apply(true, fps).count() == 0);
        assert(pacing.Apply(false, fps).count() == 0);
        assert(pacing.Apply(true, fps).count() == 0);
        assert(pacing.Apply(true, 0).count() == 0);
        assert(pacing.Apply(true, fps).count() == 0);
    }
}
'''
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, fixture)

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

    def test_input_limits_and_backend_routing(self):
        """Execute master, limiter and scope controls across input states and both native/proxy paths."""
        source = (ROOT / "src/Features/Upscaling.cpp").read_text(encoding="utf-8")
        body = braced(source, "bool Upscaling::IsAntiLag2FPSLimiterActive() const")
        body += "\n" + braced(source, "void Upscaling::OnBeforeInputPoll()")
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, ROUTING_FIXTURE.replace("PRODUCTION", body))

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
        """Clear driver caps and CPU deadlines on disable, including failures and re-enable transitions."""
        source = (ROOT / "src/Features/Upscaling/DX12SwapChain.cpp").read_text(encoding="utf-8")
        body = braced(source, "void DX12SwapChain::UpdateAntiLag2BeforeInput(")
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, DRIVER_FIXTURE.replace("PRODUCTION", body))


ROUTING_FIXTURE = r'''
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
unsigned nativeCalls = 0, nativeLimit = 0; bool nativeEnabled = false;
struct InputPacing {
    int Apply(bool enabled, unsigned limit) {
        ++nativeCalls; nativeLimit = limit; nativeEnabled = enabled; return 0;
    }
};
}
struct Upscaling {
    static constexpr unsigned kGameplayMinRenderedFPSLimit = 20;
    static constexpr unsigned kGameplayMaxRenderedFPSLimit = 240;
    bool d3d12SwapChainActive = true;
    bool UsesDLSSGFrameGen() const { return d3d12SwapChainActive && dx12SwapChain.useDLSSG; }
    struct {
        bool antiLag2Enabled = true, antiLag2FPSLimiterEnabled = true, antiLag2FPSLimiterGameplayOnly = false;
        unsigned antiLag2FPSLimit = 60;
    } settings;
    struct {
        bool useDLSSG = false, enabled = false;
        unsigned calls = 0, limit = 0;
        void UpdateAntiLag2BeforeInput(bool state, unsigned fps) { ++calls; limit = fps; enabled = state; }
    } dx12SwapChain;
    bool IsAntiLag2FPSLimiterActive() const;
    void OnBeforeInputPoll();
};
PRODUCTION
int main() {
    UI ui;
    for (unsigned requested : {0u, 19u, 20u, 60u, 144u, 240u, 241u, UINT32_MAX})
        for (bool active : {false, true})
            for (bool dlss : {false, true})
                for (bool present : {false, true})
                    for (bool paused : {false, true})
                        for (int menus = 0; menus != 8; ++menus)
                            for (unsigned controls = 0; controls != 8; ++controls) {
                                Upscaling u;
                                CS::AntiLag2::nativeCalls = 0;
                                ui.paused = paused; ui.menus = menus;
                                globals::game::ui = present ? &ui : nullptr;
                                u.d3d12SwapChainActive = active; u.dx12SwapChain.useDLSSG = dlss;
                                u.settings.antiLag2Enabled = controls & 1;
                                u.settings.antiLag2FPSLimiterEnabled = controls & 2;
                                u.settings.antiLag2FPSLimiterGameplayOnly = controls & 4;
                                u.settings.antiLag2FPSLimit = requested;
                                u.OnBeforeInputPoll();
                                assert(u.dx12SwapChain.calls == unsigned(active && !dlss));
                                assert(CS::AntiLag2::nativeCalls == unsigned(!active));
                                if (active && dlss) continue;
                                const bool applies = (controls & 1) && (controls & 2) &&
                                    (!(controls & 4) || (present && !paused && menus == 0));
                                const unsigned limit = applies ? std::clamp(requested, 20u, 240u) : 0;
                                if (active) {
                                    assert(u.dx12SwapChain.limit == limit);
                                    assert(u.dx12SwapChain.enabled == bool(controls & 1));
                                } else {
                                    assert(CS::AntiLag2::nativeLimit == limit);
                                    assert(CS::AntiLag2::nativeEnabled == applies);
                                }
                            }
}
'''

DRIVER_FIXTURE = r'''
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
int result = 0; unsigned calls = 0, lastLimit = 0; bool lastEnabled = false;
HRESULT Update(int*, bool enabled, unsigned fps) {
    ++calls; lastLimit = fps; lastEnabled = enabled; return result;
}
}
struct DX12SwapChain {
    bool antiLag2Available = false, antiLag2Enabled = false, antiLag2UpdateFailureLogged = false;
    uint32_t antiLag2LastFPSLimit = UINT32_MAX;
    int antiLag2Context = 0;
    struct {
        unsigned calls = 0, limit = 0; bool enabled = false;
        int Apply(bool active, unsigned fps) { ++calls; enabled = active; limit = fps; return 0; }
    } antiLag2InputPacing;
    void UpdateAntiLag2BeforeInput(bool enabled, uint32_t fpsLimit);
};
PRODUCTION
int main() {
    for (bool available : {false, true})
        for (int result : {-1, 0, 1}) {
            DX12SwapChain chain;
            chain.antiLag2Available = available;
            AMD::AntiLag2DX12::result = result;
            AMD::AntiLag2DX12::calls = 0; logger::warnings = 0;
            const bool states[] = {true, true, false, false, true};
            const unsigned caps[] = {60, 0, 0, 120, 144};
            for (unsigned i = 0; i != 5; ++i) {
                const bool enabled = states[i];
                const unsigned cap = enabled ? caps[i] : 0;
                chain.UpdateAntiLag2BeforeInput(enabled, caps[i]);
                assert(chain.antiLag2Enabled == (enabled && available && result >= 0));
                assert(chain.antiLag2InputPacing.enabled == (enabled && cap != 0));
                assert(chain.antiLag2InputPacing.limit == cap);
                assert(chain.antiLag2InputPacing.calls == i + 1);
                if (available) {
                    assert(AMD::AntiLag2DX12::lastEnabled == enabled);
                    assert(AMD::AntiLag2DX12::lastLimit == cap);
                }
            }
            assert(AMD::AntiLag2DX12::calls == (available ? 5u : 0u));
            assert(logger::warnings == unsigned(available && result < 0));
        }
}
'''

if __name__ == "__main__":
    unittest.main()
