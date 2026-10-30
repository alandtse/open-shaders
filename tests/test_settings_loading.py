import json
import os
import tempfile
import unittest
from pathlib import Path

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


@unittest.skipUnless(os.name == "nt", "Uses the Windows config writer")
class SettingsLoadingTests(unittest.TestCase):
    def test_native_loading_preserves_disk_and_disabled_defaults(self):
        state = (ROOT / "src/State.cpp").read_text(encoding="utf-8")
        menu = (ROOT / "src/Menu.cpp").read_text(encoding="utf-8")
        post = (ROOT / "src/Features/PostProcessing.cpp").read_text(encoding="utf-8")
        upscaling = (ROOT / "src/Features/Upscaling.cpp").read_text(encoding="utf-8")
        upscaling_header = (ROOT / "src/Features/Upscaling.h").read_text(encoding="utf-8")
        sss = (ROOT / "src/Features/SubsurfaceScattering.cpp").read_text(encoding="utf-8")
        sss_header = (ROOT / "src/Features/SubsurfaceScattering.h").read_text(encoding="utf-8")
        serialize = (ROOT / "src/Utils/Serialize.cpp").read_text(encoding="utf-8")
        keys = (ROOT / "src/Utils/SettingsKeys.cpp").read_text(encoding="utf-8")
        source = r'''
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace logger {
template<class... T> void info(T&&...) {}
template<class... T> void warn(T&&...) {}
template<class... T> void error(T&&...) {}
template<class... T> void debug(T&&...) {}
}
SETTINGS_KEYS
using InputCombo = unsigned;
struct Menu {
    struct Settings {
        std::vector<InputCombo> ToggleKey, SkipCompilationKey, EffectToggleKey,
            OverlayToggleKey, ShaderBlockPrevKey, ShaderBlockNextKey,
            CSEditorToggleKey, ScreenshotKey, Effects11ToggleKey, Effects11EditorKey;
    };
    static void OverlayInputSettings(json&, const json&, const json&);
};
BINDING_TYPE;
BINDINGS;
MENU_OVERLAY
using uint = unsigned;
#include NR_CONTEXT_PATH
struct Upscaling {
    static constexpr unsigned kFsr4RuntimeSelectionSchemaVersion = 2;
    UPSCALING_TYPES
};
UPSCALING_SERIALIZERS
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
FLOAT_ARRAY_READER
namespace nlohmann { FLOAT_SERIALIZERS }
struct SubsurfaceScattering {
    SSS_TYPES
    Settings settings;
    void LoadSettings(json&);
    void SaveSettings(json&);
};
SSS_SERIALIZERS
SSS_LOAD
SSS_SAVE
SubsurfaceScattering simulatedSSS;
Upscaling::Settings simulatedUpscaling;
struct PipelineFeature {
    bool enabled = false;
    json values = {{"Samples", 8u}, {"Strength", 1.0f}};
    std::string GetType() { return "Motion Blur"; }
    bool IsAutoEnabled() { return false; }
    void SaveSettings(json& target) { target = values; }
    void LoadSettings(json& input) { values = input; }
};
struct PostProcessing {
    json settings = {{"DisableVanillaTonemapping", 1u}};
    struct Camera {
        json settings = {{"Enabled", false}, {"Lens", {{"FNumber", 1.4f}}}};
        void SaveSettings(json& output) { output = settings; }
        void LoadSettings(json& input) { settings = input; }
    } cinematicCamera;
    std::vector<std::shared_ptr<PipelineFeature>> pipeline{std::make_shared<PipelineFeature>()};
    void ProcessSettings(json&);
};
PROCESS_SETTINGS
const std::filesystem::path fixtureRoot = FIXTURE_ROOT;
namespace Plugin { struct Version { std::string string() const { return "current"; } }; const Version VERSION; }
namespace RE { struct BSShader { enum class Type { Total = 2 }; }; }
namespace magic_enum { template<class T> int enum_integer(T value) { return static_cast<int>(value); } }
namespace Util::PathHelpers {
std::filesystem::path GetCommunityShaderPath() { return fixtureRoot; }
std::filesystem::path GetFeatureIniPath(const std::string&) { return fixtureRoot / "Fixture.ini"; }
}
struct SceneSettingsManager {
    static SceneSettingsManager* GetSingleton() { static SceneSettingsManager s; return &s; }
    struct SceneLayerGuard { SceneLayerGuard(SceneSettingsManager&) {} };
};
struct Feature {
    std::string name = "Fixture";
    bool loaded = true, installed = true;
    std::string failedLoadedMessage;
    bool IsDisabledByDefault() { return name == "Experimental Feature"; }
    bool IsAlwaysEnabled() { return false; }
    bool UsesMainSettings() { return true; }
    std::string GetShortName() { return name == "Subsurface Scattering" ? "SubsurfaceScattering" : name; }
    std::string GetName() { return name; }
    std::string GetDisplayName() { return name; }
    void Load(json& input) {
        if (name == "Subsurface Scattering") simulatedSSS.LoadSettings(input[name]);
        if (name == "Upscaling") simulatedUpscaling = input[name];
    }
    void SaveSettings(json&) {}
    void LoadSettings(json&) {}
    static std::vector<Feature*> GetFeatureList() {
        static Feature fixture, sss{"Subsurface Scattering"}, upscale{"Upscaling"}, experimental{"Experimental Feature"};
        return {&fixture, &sss, &upscale, &experimental};
    }
};
namespace FeatureIssues { void ScanForOrphanedFeatureINIs() {} }
struct SettingsOverrideManager {
    static SettingsOverrideManager* GetSingleton() { static SettingsOverrideManager m; return &m; }
    void CaptureBaseSettings(const json&) {}
    void CaptureAppliedSettings(const json&) {}
    size_t DiscoverOverrides() { return 0; }
    bool CleanupStaleUserOverrides() { return true; }
    size_t ApplyGlobalOverrides(json&) { return 0; }
    bool LoadUserOverride(const std::string&, json&) { return false; }
    bool HasFeatureOverrides(const std::string&) { return false; }
    size_t ApplyOverrides(const std::string&, json&) { return 0; }
    void PrepareUserSettings(json&) {}
    bool SaveUserEdits(const json&, const std::function<bool()>& save) { return save(); }
};
namespace globals {
struct ShaderCache { void MarkExpectedFeatureFlip() {} } cache;
ShaderCache* shaderCache = &cache;
}
struct State {
    enum ConfigMode { USER, DEFAULT };
    bool enabledClasses[1]{};
    std::map<std::string, bool> disabledFeatures;
    json defaultSettingsBaseline;
    json receivedSettings;
    json live = {{"Menu", json::object()}, {"Fixture", {{"Enabled", false}, {"Strength", 1.0f}}},
        {"Disable at Boot", json::object()}, {"Version", "current"}};
    bool throwOnLoad = false;
    State() {
        simulatedSSS.settings = {};
        simulatedUpscaling = {};
        live["General"] = {{"Enable Shaders", true}, {"Enable Disk Cache", true}, {"Enable Async", true}, {"Language", "en"}};
        live["Advanced"] = {{"Dump Shaders", false}, {"Log Level", 2}, {"Compiler Threads", 8},
            {"Background Compiler Threads", 4}, {"Use FileWatcher", true}, {"Shader Defines", ""},
            {"Content Store", true}, {"Content Store Max MB", 1024u}};
        live["Replace Original Shaders"] = {{"Lighting", true}, {"Grass", true}};
        live["Experimental Feature"] = {{"Enabled", false}};
        for (const auto& binding : kInputBindings) live["Menu"][binding.key] = 0;
        live["Menu"]["ToggleKey"] = 35u;
        live["Menu"]["CSEditorToggleKey"] = {16u, 35u};
        live["Menu"]["Effects11EditorKey"] = {17u, 35u};
    }
    void SaveToJson(json& output) {
        output = live;
        output["Version"] = "current";
        simulatedSSS.SaveSettings(output["Subsurface Scattering"]);
        output["Upscaling"] = simulatedUpscaling;
    }
    void LoadFromJson(json& input) {
        if (throwOnLoad) throw std::runtime_error("fixture load failure");
        receivedSettings = input;
        disabledFeatures = input["Disable at Boot"].get<std::map<std::string, bool>>();
        live = input;
        live["Fixture"]["Enabled"] = input["Fixture"].value("Enabled", false);
        live["Fixture"]["Strength"] = input["Fixture"].value("Strength", 1.0f);
    }
    void Load(ConfigMode = USER, bool = true);
    void Save(ConfigMode = USER, bool = true);
    bool SaveFeaturePreference(const json&);
};
static std::string GetConfigPath(State::ConfigMode mode) {
    return (fixtureRoot / (mode == State::USER ? "SettingsUser.json" : "SettingsDefault.json")).string();
}
WRITE_FILE
WRITE_ATOMIC
LOAD
SAVE
SAVE_PREFERENCE
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main() {
    PostProcessing post;
    auto& feature = *post.pipeline.front();
    json oldPipeline = {{"Motion Blur", {{"settings", {{"Strength", 2}}}}}};
    post.ProcessSettings(oldPipeline);
    check(!feature.enabled, "Missing enabled must not turn on a disabled pipeline entry");
    check(feature.values["Strength"] == 2.0f, "Valid deferred settings survive");
    json malformedPipeline = {{"Motion Blur", {{"enabled", "yes"}, {"settings", {{"Samples", "bad"}}}}}};
    post.ProcessSettings(malformedPipeline);
    check(!feature.enabled && feature.values["Samples"] == 8, "Malformed deferred fields retain defaults");
    oldPipeline["Motion Blur"]["enabled"] = true;
    post.ProcessSettings(oldPipeline);
    check(feature.enabled, "Explicit enabled pipeline entries remain enabled");
    json cameraSettings = {{"cinematic_camera", {{"Enabled", "yes"}, {"Lens", {{"FNumber", 2.8f}}}}}};
    post.ProcessSettings(cameraSettings);
    check(post.cinematicCamera.settings["Enabled"] == false,
        "Malformed camera enablement cannot opt in to the new pipeline");
    check(post.cinematicCamera.settings["Lens"]["FNumber"] == 2.8f,
        "Valid camera settings survive alongside malformed fields");

    const auto userPath = GetConfigPath(State::USER);
    const auto defaultPath = GetConfigPath(State::DEFAULT);
    const std::string oldFile = R"({ "Version": "old", "Fixture": { "Strength": 2 }, "Menu": { "ToggleKey": [17, 35], "CSEditorToggleKey": 36, "SkipCompilationKey": -1, "Effects11EditorKey": [17, 36] } })";
    check(WriteConfigFile(userPath, oldFile), "Create old user fixture");
    check(WriteConfigFile(defaultPath, R"({"Fixture":{"Enabled":true}})"), "Create stale default fixture");
    State state;
    state.Load();
    check(read(userPath) == oldFile, "Version mismatch must preserve the original bytes");
    check(state.live["Fixture"]["Enabled"] == false, "Missing settings use current defaults, not stale disk defaults");
    check(state.live["Fixture"]["Strength"] == 2.0f, "Valid old values apply");
    check(!state.receivedSettings["Fixture"].contains("Enabled"),
        "Feature loaders can distinguish missing keys for legacy migrations");
    check(state.live["Menu"]["ToggleKey"] == json{17u, 35u}, "Menu accepts a combo instead of a scalar");
    check(state.live["Menu"]["CSEditorToggleKey"] == 36u, "Menu accepts a scalar instead of a combo");
    check(state.live["Menu"]["Effects11EditorKey"] == json{17u, 36u}, "Effects 11 editor imports saved combos");
    check(state.live["Menu"]["SkipCompilationKey"] == 0, "Invalid binding restores the pristine default");
    state.live["Fixture"]["Enabled"] = true;
    state.Load();
    check(state.live["Fixture"]["Enabled"] == false, "Reload does not capture unsaved live settings as defaults");
    check(read(userPath) == oldFile, "Reload preserves original bytes");
    state.throwOnLoad = true;
    state.Load();
    check(read(userPath) == oldFile, "Load failure must not overwrite the user file");
    state.throwOnLoad = false;
    const auto oldSettings = json::parse(oldFile);
    for (const json patch : {
        json{{"Disable at Boot", {{"Fixture", true}}}},
        json{{"Disable at Boot", {{"Fixture", false}}}},
        json{{"Favorites", {{"Fixture", true}}}},
        json{{"Favorites", {{"Fixture", false}}}}
    }) {
        auto legacySettings = oldSettings;
        legacySettings["Obsolete Feature"] = {{"Unknown Field", "preserve me"}};
        legacySettings["Fixture"]["Enabled"] = "invalid";
        check(WriteConfigFile(userPath, legacySettings.dump()), "Restore legacy preference fixture");
        check(state.SaveFeaturePreference(patch), "Preference change saves successfully");
        legacySettings.merge_patch(patch);
        check(json::parse(read(userPath)) == legacySettings,
            "Preference saves change only their patch, not the version or other settings");
    }
    for (const std::string malformed : {"{broken", "{\"Fixture\":{\"Strength\":1e400}}", "null", "[]"}) {
        check(WriteConfigFile(userPath, malformed), "Write malformed fixture");
        state.Load();
        check(read(userPath) == malformed, "Invalid documents remain unchanged until Save");
        check(state.live["Fixture"]["Enabled"] == false, "Invalid documents keep default enablement");
        check(!state.SaveFeaturePreference(json{{"Favorites", {{"Fixture", true}}}}),
            "Preference save fails for invalid JSON");
        check(read(userPath) == malformed, "Failed preference save preserves the original bytes");
    }
    check(WriteConfigFile(userPath, oldFile), "Restore old fixture");
    state.Load();
    state.Save();
    const auto saved = json::parse(read(userPath));
    check(saved["Version"] == "current", "Explicit Save updates the version");
    check(saved["Fixture"]["Enabled"] == false && saved["Fixture"]["Strength"] == 2.0f,
        "Explicit Save persists validated effective settings");
    const auto fixtures = json::parse(read(CS_FIXTURE_PATH));
    for (const auto& fixture : fixtures["cases"]) {
        for (const bool characterLighting : {false, true}) {
            auto imported = fixture["settings"];
            imported["Subsurface Scattering"]["EnableCharacterLighting"] = unsigned(characterLighting);
            const auto originalBytes = imported.dump(1);
            check(WriteConfigFile(userPath, originalBytes), "Write simulated Community Shaders config");
            check(WriteConfigFile(defaultPath, R"({"Upscaling":{"neuralRenderingEnabled":true,"enableDLSSFrameGen":true,"fsr4RuntimeEnable":true}})"),
                "Seed stale defaults from another install");
            State migrated;
            migrated.Load();
            check(read(userPath) == originalBytes, "Community Shaders file is byte-identical after import");
            check(simulatedSSS.settings.EnableCharacterLighting == unsigned(characterLighting),
                "Community Shaders explicit character lighting choice is respected");
            check(simulatedSSS.settings.BaseProfile.BlurRadius == 1.25f, "Actual SSS deserializer retains the profile");
            check(simulatedSSS.settings.ScatterMode == 2, "Absent modern SSS fields use current defaults");
            check(!simulatedUpscaling.neuralRenderingEnabled && !simulatedUpscaling.enableDLSSFrameGen,
                "Absent Open Shaders opt-ins stay off using actual current Upscaling settings");
            check(simulatedUpscaling.fsr4RuntimeEnable, "Runtime upscaler upgrades default to enabled");
            check(json(simulatedUpscaling.neuralRenderingContexts) == json(NR::Context::Profiles{}),
                "Old configs keep the new dialogue profiles at their unchanged defaults");
            check(migrated.live["Menu"]["ToggleKey"] == 36u, "Old and current Community Shaders menu keys survive");
            check(migrated.live["Menu"]["Effects11EditorKey"] == json{17u, 35u}, "Missing editor binding keeps its current default");
            check(migrated.live["General"]["Enable Async"] == false, "Explicit core preference survives");
            check(migrated.live["Replace Original Shaders"]["Grass"] == false, "Shader selection survives");
            check(migrated.disabledFeatures["Experimental Feature"], "New disabled-by-default features remain disabled");
            if (imported.contains("Upscaling")) {
                check(simulatedUpscaling.qualityMode == 2 && simulatedUpscaling.frameGenerationMode == 0,
                    "Current Community Shaders quality and frame generation choices survive");
                check(simulatedSSS.settings.CharacterLightingStrength == 1.75f && simulatedSSS.settings.BurleySamples == 12,
                    "Current Community Shaders SSS strength and samples survive");
            }
            const json favoritePatch = {{"Favorites", {{"SubsurfaceScattering", true}}}};
            check(migrated.SaveFeaturePreference(favoritePatch), "Favorite simulated import");
            imported.merge_patch(favoritePatch);
            check(json::parse(read(userPath)) == imported, "Favorite does not refresh a Community Shaders file");
            migrated.Save();
            const auto normalized = json::parse(read(userPath));
            check(normalized["Version"] == "current", "Save upgrades the imported version");
            migrated.Load();
            check(simulatedSSS.settings.EnableCharacterLighting == unsigned(characterLighting), "Saved import round trips");
            check(json::parse(read(userPath)) == normalized, "Reload does not rewrite the saved import");
        }
        auto obscure = fixture["settings"];
        obscure["Subsurface Scattering"]["EnableCharacterLighting"] = true;
        obscure["Subsurface Scattering"]["CharacterLightingStrength"] = "strong";
        obscure["Subsurface Scattering"]["BurleySamples"] = -1;
        obscure["Subsurface Scattering"]["BaseProfile"]["Strength"] = {1, "invalid", 3};
        obscure["Upscaling"]["neuralRenderingEnabled"] = "true";
        obscure["Upscaling"]["qualityMode"] = 4294967296ULL;
        obscure["Upscaling"]["neuralRenderingContexts"] = {
            {"normal", {{"run", false}}},
            {"dialogue", {{"run", "yes"}, {"scope", -1}, {"region", "full"}}}
        };
        obscure["Unknown Obsolete Feature"] = {{"Enabled", true}};
        const auto obscureBytes = obscure.dump(1);
        check(WriteConfigFile(userPath, obscureBytes), "Write malformed Community Shaders variant");
        State filtered;
        filtered.Load();
        check(read(userPath) == obscureBytes, "Malformed fields do not cause an automatic file refresh");
        check(simulatedSSS.settings.EnableCharacterLighting == 0 && simulatedSSS.settings.BurleySamples == 16,
            "Malformed flags and unsigned values retain actual defaults");
        check(simulatedSSS.settings.CharacterLightingStrength == 1.0f && simulatedSSS.settings.BaseProfile.Strength.x == 0.48f,
            "Malformed scalar and vector fields retain actual defaults");
        check(simulatedSSS.settings.BaseProfile.BlurRadius == 1.25f, "Malformed neighbors do not discard valid values");
        check(!simulatedUpscaling.neuralRenderingEnabled && simulatedUpscaling.qualityMode == 1,
            "Malformed newly added options cannot enable NR or overflow enum storage");
        check(!simulatedUpscaling.neuralRenderingContexts.normal.run &&
            json(simulatedUpscaling.neuralRenderingContexts.dialogue) == json(NR::Context::ContextProfile{}),
            "Malformed dialogue values retain defaults without discarding a valid normal profile");
        check(!filtered.receivedSettings.contains("Unknown Obsolete Feature"), "Unknown roots are not applied");
        obscure = fixture["settings"];
        obscure["Subsurface Scattering"].erase("EnableCharacterLighting");
        obscure["Menu"]["ToggleKey"] = 37;
        const auto missingBytes = obscure.dump(1);
        check(WriteConfigFile(userPath, missingBytes), "Write partial Community Shaders variant");
        filtered.Load();
        check(simulatedSSS.settings.EnableCharacterLighting == 0, "Absent character lighting never opts in");
        check(filtered.live["Menu"]["ToggleKey"] == 37u, "Current hotkey names win over legacy aliases");
        check(read(userPath) == missingBytes, "Partial Community Shaders file remains unchanged");
    }
    const json profiles = {
        {"normal", {{"run", false}, {"scope", 0}, {"region", 0}}},
        {"dialogue", {{"run", true}, {"scope", 2}, {"region", 1}}}
    };
    for (const bool enabled : {false, true}) {
        json imported = {{"Version", "old"}, {"Upscaling", {
            {"neuralRenderingEnabled", enabled}, {"neuralRenderingContexts", profiles}}}};
        const auto originalBytes = imported.dump(1);
        check(WriteConfigFile(userPath, originalBytes), "Write saved dialogue profiles");
        State migrated;
        migrated.Load();
        check(read(userPath) == originalBytes, "Loading dialogue profiles preserves the original bytes");
        check(simulatedUpscaling.neuralRenderingEnabled == enabled &&
            json(simulatedUpscaling.neuralRenderingContexts) == profiles,
            "Dialogue profiles and explicit NR enablement import independently");
        const json favoritePatch = {{"Favorites", {{"Upscaling", true}}}};
        check(migrated.SaveFeaturePreference(favoritePatch), "Favorite imported dialogue profiles");
        imported.merge_patch(favoritePatch);
        check(json::parse(read(userPath)) == imported, "Favorites preserve imported dialogue profiles without normalizing them");
        migrated.Save();
        const auto normalized = json::parse(read(userPath));
        check(normalized["Upscaling"]["neuralRenderingContexts"] == profiles,
            "Explicit Save preserves dialogue profiles");
        migrated.Load();
        check(simulatedUpscaling.neuralRenderingEnabled == enabled &&
            json(simulatedUpscaling.neuralRenderingContexts) == profiles,
            "Saved dialogue profiles and NR enablement round trip");
        check(json::parse(read(userPath)) == normalized, "Reload does not rewrite saved dialogue profiles");
    }
}
'''
        tokens = {
            "SETTINGS_KEYS": keys.replace('"Utils/SettingsPatch.h"', json.dumps((ROOT / "src/Utils/SettingsPatch.h").as_posix())),
            "BINDING_TYPE": braced(menu, "struct InputBindingSetting"),
            "BINDINGS": braced(menu, "constexpr InputBindingSetting kInputBindings[]"),
            "MENU_OVERLAY": braced(menu, "void Menu::OverlayInputSettings("),
            "PROCESS_SETTINGS": braced(post, "void PostProcessing::ProcessSettings("),
            "NR_CONTEXT_PATH": json.dumps((ROOT / "src/Features/Upscaling/NeuralRendering/ContextProfile.h").as_posix()),
            "UPSCALING_TYPES": "\n".join(braced(upscaling_header, token) + ";" for token in
                                         ("enum class UpscaleMethod", "enum class QualityMode", "struct Settings")),
            "UPSCALING_SERIALIZERS": upscaling[upscaling.index("namespace NR"):upscaling.index("decltype(&D3D11")],
            "SSS_TYPES": "\n".join(braced(sss_header, token) + ";" for token in
                                   ("struct DiffusionProfile", "enum ScatterMode", "struct Settings")),
            "SSS_SERIALIZERS": sss[sss.index("NLOHMANN_DEFINE_TYPE"):sss.index("void SubsurfaceScattering::DrawSettings")],
            "SSS_LOAD": braced(sss, "void SubsurfaceScattering::LoadSettings("),
            "SSS_SAVE": braced(sss, "void SubsurfaceScattering::SaveSettings("),
            "FLOAT_ARRAY_READER": "template<size_t N>\n" + braced(serialize, "bool ReadFloatArray("),
            "FLOAT_SERIALIZERS": "\n".join(braced(serialize, f"void {direction}_json({signature}")
                                          for kind in ("float3", "float4")
                                          for direction, signature in (("to", f"json& j, const {kind}& v"),
                                                                       ("from", f"const json& j, {kind}& v"))),
            "CS_FIXTURE_PATH": json.dumps((ROOT / "tests/fixtures/settings/community-shaders.json").as_posix()),
            "WRITE_FILE": braced(state, "static bool WriteConfigFile("),
            "WRITE_ATOMIC": braced(state, "static bool WriteConfigAtomically("),
            "LOAD": braced(state, "void State::Load("),
            "SAVE": braced(state, "void State::Save("),
            "SAVE_PREFERENCE": braced(state, "bool State::SaveFeaturePreference("),
        }
        for token in sorted(tokens, key=len, reverse=True):
            replacement = tokens[token]
            source = source.replace(token, replacement)
        with tempfile.TemporaryDirectory(prefix="settings-load-fixture-") as fixture:
            source = source.replace("FIXTURE_ROOT", json.dumps(Path(fixture).as_posix()))
            library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
            runtime.SceneSettingsRuntimeTests.compile_and_run(self, source, imgui_root=library_root)


if __name__ == "__main__":
    unittest.main()
