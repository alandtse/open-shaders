import re
import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


class Effects11SkyTests(unittest.TestCase):
    def test_paused_frames_preserve_engine_sky_scale(self):
        effects = (ROOT / "src/Features/Effects11.cpp").read_text(encoding="utf-8-sig")
        header = (ROOT / "src/Features/Effects11.h").read_text(encoding="utf-8-sig")
        sky = (ROOT / "package/Shaders/Sky.hlsl").read_text(encoding="utf-8-sig")
        game = (ROOT / "src/Utils/Game.h").read_text(encoding="utf-8-sig")
        source = r'''
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cstdint>
#include <string_view>
using uint = uint32_t;
struct float3 {
    float x, y, z;
    float3(float v = 0) : x(v), y(v), z(v) {}
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
    void operator*=(float v) { x *= v; y *= v; z *= v; }
};
struct float4 { float x, y, z, w; };
namespace RE { struct TESWeather { enum ColorTypes { kHorizon }; }; }
namespace globals {
struct State { uint frameCount = 0; } stateStorage;
auto* state = &stateStorage;
namespace game {
struct Sky { struct Color { float red, green, blue; } skyColor[1]; };
Sky* sky = nullptr;
struct ImageSpaceManager {
    struct { struct { struct { float skyScale = 0.5f; } hdr; } baseData; } data;
} imageSpaceStorage;
auto* imageSpaceManager = &imageSpaceStorage;
}
}
#define GET_INSTANCE_MEMBER_VRPTR(name, object) auto& name = object->name
FRAME_CHECKER;
bool FrameChecker::IsNewFrame() { return IsNewFrame(globals::state->frameCount); }
struct SettingManager {
    float gradient = 2.0f;
    bool disableWrongSkyMath = false;
    static SettingManager& GetSingleton() { static SettingManager manager; return manager; }
    template<class T> T GetValue(std::string_view key, std::string_view) {
        return key == "DisableWrongSkyMath" ? T(disableWrongSkyMath) : T(1);
    }
    float GetInterpolatedTimeOfDayValue(std::string_view key, std::string_view) {
        return key == "GradientIntensity" ? gradient : 1.0f;
    }
    float4 GetInterpolatedColorTimeOfDayValue(std::string_view, std::string_view) { return {1, 1, 1, 1}; }
};
struct Effects11 {
    PER_FRAME;
    bool loaded = true, enableEffect = true;
    struct { bool IgnorePresetParticles = false; } settings;
    FrameChecker perFrameCacheChecker;
    PerFrame perFrameCache{};
    void CheckCommonData() {}
    bool IsRainEnabled() { return false; }
    void UpdateSkyScattering(PerFrame&) {}
    PerFrame GetCommonBufferData();
    void Prepass();
};
METHODS
namespace SharedData { Effects11::PerFrame enbSettings; }
float shadeSky(float engineScale) {
    struct { float3 yyy; } PParams{engineScale};
    SKY_SCALE
    return skyScale.x;
}
int main() {
    Effects11 effects;
    auto& settings = SettingManager::GetSingleton();
    auto& engineScale = globals::game::imageSpaceManager->data.baseData.hdr.skyScale;
    const auto renderFrame = [&](float expected) {
        ++globals::state->frameCount;
        effects.Prepass();
        SharedData::enbSettings = effects.GetCommonBufferData();
        assert(shadeSky(engineScale) == expected);
        SharedData::enbSettings = effects.GetCommonBufferData();
        assert(shadeSky(engineScale) == expected);
    };
    for (int frame = 0; frame < 1000; ++frame) {
        renderFrame(1.0f);
        assert(engineScale == 0.5f);
    }
    settings.gradient = 3.0f;
    renderFrame(1.5f);
    engineScale = 0.25f;
    renderFrame(0.75f);
    settings.disableWrongSkyMath = true;
    renderFrame(0.0f);
    assert(engineScale == 0.25f);
    effects.enableEffect = false;
    renderFrame(0.25f);
    effects.enableEffect = true;
    settings.disableWrongSkyMath = false;
    renderFrame(0.75f);
    effects.loaded = false;
    renderFrame(0.25f);
    assert(engineScale == 0.25f);
}
'''
        source = source.replace("FRAME_CHECKER", braced(game, "class FrameChecker"))
        source = source.replace("PER_FRAME", braced(header, "struct alignas(16) PerFrame"))
        prepass = "void Effects11::Prepass() {}"
        if "void Effects11::Prepass()" in effects:
            prepass = braced(effects, "void Effects11::Prepass()")
        source = source.replace("METHODS", prepass + "\n" + braced(
            effects, "Effects11::PerFrame Effects11::GetCommonBufferData()"))
        source = source.replace("SKY_SCALE", re.search(
            r"float3 skyScale = PParams\.yyy;.*?(?=\s*bool composeAuthoredSky)", sky, re.S).group())
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, "#define EFFECTS11\n" + source)


if __name__ == "__main__":
    unittest.main()
