import os
import unittest

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import ROOT, braced


class SceneSettingsControlTests(unittest.TestCase):
    def test_uncatalogued_controls_preserve_navigation_and_nested_widgets(self):
        library_root = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        if os.name != "nt" or not (library_root / "lib/imgui.lib").exists():
            self.skipTest("Uses the Windows build's ImGui library")
        hooks = (ROOT / "src/SceneSettingsUIHooks.cpp").read_text(encoding="utf-8")
        source = r'''
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
struct Feature { std::string_view GetShortName() const { return "Fixture"; } } feature;
namespace SceneSettingsCatalog {
enum class AggregateSemantic { None, Vector };
struct Choice { std::string_view displayName, displayNameKey; };
struct SettingMetadata {
    std::string_view featureShortName = "Fixture", settingPath, settingKey = "Value";
    std::string_view serializedPath, serializedKey = "Value", displayName = "Known", displayNameKey, controlScope;
    AggregateSemantic aggregateSemantic = AggregateSemantic::None;
    std::int8_t aggregateStart = 0;
    std::uint8_t aggregateCount = 0;
    const Choice* choices = nullptr;
    std::size_t choiceCount = 0;
};
struct NavigationControlMetadata { std::string_view featureShortName, displayName, displayNameKey; };
struct VirtualAggregateControlMetadata { std::string_view featureShortName, pushId, itemLabel; };
std::vector<SettingMetadata> entries(1);
std::vector<NavigationControlMetadata> navigation{{"Fixture", "More Options", ""}};
std::vector<VirtualAggregateControlMetadata> virtualControls{{"Fixture", "Vector", "##Unified"}};
const auto& GetSettings() { return entries; }
const auto& GetNavigationControls() { return navigation; }
const auto& GetVirtualAggregateControls() { return virtualControls; }
bool IsSceneControllable(const SettingMetadata&) { return true; }
float storedValue = 0.5f;
const SettingMetadata* FindSettingForControl(Feature*, const void* address) {
    return address == &storedValue ? &entries[0] : nullptr;
}
}
bool g_sceneSettingsActive = false, g_featureSceneEditing = false;
bool allowed = true, editable = true, active = false;
Feature* g_currentFeature = &feature;
unsigned int g_controlDetourDepth = 0;
int finished = 0, mutations = 0;
const char* T(std::string_view, const char* fallback) { return fallback; }
namespace Util {
const void* storage = nullptr;
const void* GetActiveControlStorageAddress() { return storage; }
float GetUIScale() { return 1.0f; }
}
struct SceneSettingsManager {
    static bool IsSceneSettingAllowed(std::string_view, std::string_view, std::string_view) { return allowed; }
};
bool ShouldBlockSetting(const SceneSettingsCatalog::SettingMetadata&) {
    return g_featureSceneEditing ? !allowed || !editable : active;
}
bool ShouldOutlineSetting(const SceneSettingsCatalog::SettingMetadata&) { return false; }
template<class T> const SceneSettingsCatalog::SettingMetadata* FindBlockedAggregateSetting(const T&) {
    return ShouldBlockSetting(SceneSettingsCatalog::entries[0]) ? &SceneSettingsCatalog::entries[0] : nullptr;
}
void ClearControlledItem() {}
void FinishControlledItem() { ++finished; }
bool TrackFeatureSettingMutation(bool changed) { mutations += changed; return changed; }
auto g_checkbox = static_cast<bool(*)(const char*, bool*)>(&ImGui::Checkbox);
auto g_button = static_cast<bool(*)(const char*, const ImVec2&)>(&ImGui::Button);
auto g_beginCombo = static_cast<bool(*)(const char*, const char*, ImGuiComboFlags)>(&ImGui::BeginCombo);
HELPERS
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
bool disabled() { return (GImGui->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0; }
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 1000);
    unsigned char* pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (bool editing : {false, true}) {
        g_featureSceneEditing = editing;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(700, 950));
        ImGui::Begin("Fixture");
        float temporary = 0.5f;
        bool toggle = false;
        const float alpha = ImGui::GetStyle().Alpha;
        int before = finished;
        const bool changed = DrawControl("##Uncatalogued", &temporary, [&] {
            check(ImGui::GetStyle().Alpha == (editing ? alpha * ImGui::GetStyle().DisabledAlpha : alpha), "Unavailable controls are visibly dimmed");
            ImGui::SliderFloat("##Uncatalogued", &temporary, 0, 1);
            return true;
        });
        check(disabled() == editing && changed == !editing, "Unknown values are disabled only during scene editing");
        check(finished - before == (editing ? 1 : 0), "Unavailable values receive the existing tooltip");
        check(temporary == 0.5f && ImGui::GetStyle().Alpha == alpha, "Drawing restores style and preserves data");
        CheckboxDetour("Unknown Toggle", &toggle);
        check(disabled() == editing, "Unknown toggles follow scene availability");
        CheckboxDetour("More Options", &toggle);
        check(!disabled(), "Discovered UI visibility toggles remain usable");
        for (bool blocked : {false, true}) {
            editable = !blocked;
            g_sceneSettingsActive = !editing;
            active = blocked;
            DrawControl("Known", &SceneSettingsCatalog::storedValue, [&] {
                return DrawControl("##Component", &temporary, [&] {
                    return ImGui::SliderFloat("##Component", &temporary, 0, 1);
                });
            });
            check(disabled() == blocked, "Nested editors inherit the owning setting's availability");
        }
        editable = true; active = false;
        allowed = false;
        DrawControl("Known", &SceneSettingsCatalog::storedValue, [&] { return ImGui::SliderFloat("Known", &temporary, 0, 1); });
        check(disabled() == editing, "Blacklisted values stay unavailable only in scene editing");
        allowed = true;
        DrawControl("Known", &temporary, [&] { return ImGui::SliderFloat("Known", &temporary, 0, 1); });
        check(!disabled(), "Recognized proxy editors remain editable even without a direct storage match");
        ImGui::PushID("Vector");
        DrawControl("##Unified", &temporary, [&] { return ImGui::SliderFloat("##Unified", &temporary, 0, 1); });
        check(!disabled(), "Recognized virtual aggregate controls remain editable");
        SceneSettingsCatalog::virtualControls.push_back(SceneSettingsCatalog::virtualControls[0]);
        DrawControl("##Unified", &temporary, [&] { return ImGui::SliderFloat("##Unified", &temporary, 0, 1); });
        check(disabled() == editing, "Ambiguous virtual controls cannot authorize an unknown edit");
        SceneSettingsCatalog::virtualControls.pop_back();
        ImGui::PopID();
        ButtonDetour("Next Page", ImVec2(0, 0));
        check(!disabled(), "Unbound navigation buttons remain available");
        if (BeginComboDetour("Browse", "Current", 0)) ImGui::EndCombo();
        check(!disabled(), "Unbound browsing dropdowns remain available");
        Util::storage = &temporary;
        if (BeginComboDetour("Uncatalogued Choice", "Current", 0)) ImGui::EndCombo();
        check(disabled() == editing, "Uncatalogued dropdowns with setting storage are disabled");
        Util::storage = &SceneSettingsCatalog::storedValue;
        if (BeginComboDetour("Stored Choice", "Current", 0)) ImGui::EndCombo();
        check(!disabled(), "Dropdowns using shared storage context resolve normally");
        Util::storage = nullptr;
        char search[32] = {};
        ImGui::InputTextWithHint("##Search", "Search", search, sizeof(search));
        check(!disabled(), "Search fields remain available after a blocked editor");
        if (ImGui::BeginTabBar("Tabs")) {
            if (ImGui::BeginTabItem("Details")) ImGui::EndTabItem();
            check(!disabled(), "Tab navigation remains available");
            ImGui::EndTabBar();
        }
        ImGui::BeginDisabled();
        DrawControl("Known", &temporary, [&] { return ImGui::SliderFloat("Known", &temporary, 0, 1); });
        check(disabled(), "Existing outer disabled state is preserved");
        ImGui::EndDisabled();
        check(g_controlDetourDepth == 0 && GImGui->DisabledStackSize == 0, "Control scopes restore all stacks");
        ImGui::End(); ImGui::Render();
    }
    ImGui::DestroyContext();
}
'''
        helpers = []
        for declaration in (
                "bool IsSceneControlGuardActive(", "std::string_view GetVisibleLabel(",
                "enum class ControlLabelMatch", "ControlLabelMatch MatchLocalizedLabel(",
                "bool IsSameLogicalControl(", "ControlLabelMatch MatchSettingLabel(",
                "const SceneSettingsCatalog::SettingMetadata* FindUniqueBlockedSettingForLabel(",
                "struct RegisteredVirtualControlMatch", "RegisteredVirtualControlMatch FindRegisteredVirtualControlSetting(",
                "const SceneSettingsCatalog::SettingMetadata* FindVirtualControlSetting(",
                "const SceneSettingsCatalog::SettingMetadata* FindControlSetting(",
                "struct ControlDetourScope", "struct SettingOutlineGuard",
                "bool DrawControl(", "bool CheckboxDetour(", "bool ButtonDetour(", "bool BeginComboDetour("):
            helper = braced(hooks, declaration)
            if declaration.startswith(("struct ", "enum ")):
                helper += ";"
            if declaration == "bool DrawControl(":
                helper = "template<class Draw>\n" + helper
            helpers.append(helper)
        runtime.SceneSettingsRuntimeTests().compile_and_run(source.replace("HELPERS", "\n".join(helpers)), imgui_root=library_root)


if __name__ == "__main__":
    unittest.main()
