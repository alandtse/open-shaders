import unittest
from pathlib import Path

import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


ROOT = Path(__file__).parents[1]


class EditorStartupTests(unittest.TestCase):
    def test_native_editor_ids_and_automatic_loading(self):
        form = (ROOT / "src/Utils/Form.cpp").read_text(encoding="utf-8")
        widget = (ROOT / "src/CSEditor/Widget.h").read_text(encoding="utf-8")
        factory = (ROOT / "src/CSEditor/WeatherUtils.h").read_text(encoding="utf-8")
        editor = (ROOT / "src/CSEditor/EditorWindow.cpp").read_text(encoding="utf-8")
        feature = (ROOT / "src/Features/CSEditor.cpp").read_text(encoding="utf-8")
        source = r'''
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int lockDepth = 0, mapRequests = 0, mapVisits = 0, savedLoads = 0;
namespace RE {
using FormID = unsigned;
enum class FormType { Other, ImageSpace, VolumetricLighting, ShaderParticleGeometryData, LensFlare, ReferenceEffect };
struct File { const char* GetFilename() const { return "Fixture.esm"; } };
struct TESForm {
    std::string direct;
    bool nullDirect = false;
    unsigned localID = 1;
    const char* GetFormEditorID() const { return nullDirect ? nullptr : direct.c_str(); }
    static auto GetAllFormsByEditorID();
    unsigned GetLocalFormID() const { return localID; }
    File* GetFile(int) const { static File file; return &file; }
    FormType GetFormType() const { return FormType::Other; }
};
struct Name {
    std::string value;
    const char* c_str() const {
        check(lockDepth == 1, "Map strings must be copied under the read lock");
        ++mapVisits;
        return value.c_str();
    }
};
using FormMap = std::vector<std::pair<Name, TESForm*>>;
FormMap* activeMap = nullptr;
auto TESForm::GetAllFormsByEditorID() { ++mapRequests; return std::pair{activeMap, &lockDepth}; }
struct BSReadLockGuard {
    explicit BSReadLockGuard(int*) { ++lockDepth; }
    ~BSReadLockGuard() { --lockDepth; }
};
FORM_TYPES
struct TESDataHandler {
    static TESDataHandler* GetSingleton() { static TESDataHandler handler; return &handler; }
    template<class T> std::vector<T*>& GetFormArray() { static std::vector<T*> forms; return forms; }
};
}
#include "FORM_HEADER"
FORMAT_SPID
FORM_FILE_KEY
FORM_INDEX
FORM_EDITOR_ID
class Widget {
public:
    RE::TESForm* form = nullptr;
    mutable std::string cachedEditorID;
    std::string cachedSaveKey;
    mutable bool isFallbackEditorID = false;
    virtual ~Widget() = default;
    CACHE_FORM_DATA
    GET_EDITOR_ID
    GET_SAVE_KEY
    void Load(bool userInitiated) {
        check(!userInitiated, "Startup settings must load automatically");
        check(lockDepth == 0, "Map lock must be released before loading settings");
        check(GetSaveKey() == std::format("0x{:X}~Fixture.esm", form->localID), "Saved SPID identity must be preserved");
        check(GetEditorID() == "alias" + std::to_string(form->localID), "Legacy editor-ID identity must be preserved");
        ++savedLoads;
    }
};
WIDGET_TYPES
struct SimpleFormWidget : Widget {};
namespace WidgetFactory {
POPULATE_WIDGETS
POPULATE_SIMPLE_WIDGETS
}
struct PaletteWindow {
    static PaletteWindow* GetSingleton() { static PaletteWindow palette; return &palette; }
    void Load() {}
};
struct EditorWindow {
    WIDGET_COLLECTIONS
    static EditorWindow* GetSingleton() { static EditorWindow editor; return &editor; }
    static bool CanBeOpen() { return true; }
    void Load() {}
    void InvalidateJsonAttachmentCache() {}
    void SetupResources();
};
EDITOR_SETUP
struct CSEditor {
    bool s_dataAvailable = false, s_resourcesInitialized = false, savedFilesExist = true;
    bool HasWidgetJsonFiles() { return savedFilesExist; }
    void UpdateWeatherLockAndTime() {}
    bool ShouldPreloadEditorResources();
    void EnsureDataLoaded();
    void Prepass();
};
SHOULD_PRELOAD
ENSURE_DATA_LOADED
PREPASS
int main() {
    RE::TESForm direct;
    direct.direct = "direct";
    Util::FormEditorIDIndex directOnly;
    check(Util::GetFormEditorID(nullptr, &directOnly).empty(), "Null forms stay empty");
    check(Util::GetFormEditorID(&direct, &directOnly) == "direct" && mapRequests == 0,
          "Direct-only batches must not access the fallback map");

    std::array<RE::TESForm, 64> forms;
    for (unsigned i = 0; i < forms.size(); ++i) {
        if (i % 3 == 0) forms[i].direct = "direct" + std::to_string(i);
        forms[i].nullDirect = i % 5 == 0;
    }
    std::mt19937 random(937);
    RE::FormMap entries;
    for (unsigned i = 0; i < 256; ++i)
        entries.push_back({{"alias" + std::to_string(i)}, &forms[random() % 60]});
    RE::activeMap = &entries;
    for (int trial = 0; trial < 100; ++trial) {
        std::shuffle(entries.begin(), entries.end(), random);
        Util::FormEditorIDIndex index;
        const int before = mapRequests;
        for (const auto& item : forms) {
            std::string expected = !item.nullDirect ? item.direct : "";
            if (expected.empty()) {
                for (const auto& [name, entry] : entries)
                    if (entry == &item) { expected = name.value; break; }
            }
            check(Util::GetFormEditorID(&item, &index) == expected, "Direct IDs and first aliases must retain precedence");
        }
        for (int i = 0; i < 100; ++i)
            check(Util::GetFormEditorID(&forms[62], &index).empty(), "Repeated missing IDs must stay empty");
        check(mapRequests == before + 1 && lockDepth == 0, "One snapshot per batch, with no retained lock");
    }
    RE::TESForm changing;
    entries.clear();
    entries.push_back({{"first"}, &changing});
    Util::FormEditorIDIndex oldBatch;
    check(Util::GetFormEditorID(&changing, &oldBatch) == "first", "Initial fallback found");
    entries.front().first.value = "new";
    check(Util::GetFormEditorID(&changing, &oldBatch) == "first", "Snapshot owns copied strings");
    check(Util::GetFormEditorID(&changing) == "new", "Unbatched lookup must stay live");
    Util::FormEditorIDIndex nextBatch;
    check(Util::GetFormEditorID(&changing, &nextBatch) == "new", "Next initialization refreshes the map");
    changing.direct = "lateDirect";
    check(Util::GetFormEditorID(&changing, &oldBatch) == "lateDirect", "Late direct IDs override cached aliases");

    RE::activeMap = nullptr;
    Util::FormEditorIDIndex noMap;
    RE::TESForm missing;
    const int beforeMissing = mapRequests;
    for (int i = 0; i < 100; ++i)
        check(Util::GetFormEditorID(&missing, &noMap).empty(), "Absent maps must return empty");
    check(mapRequests == beforeMissing + 1, "Absent map must not be retried for each form in a batch");
    Widget lateWidget;
    lateWidget.form = &missing;
    lateWidget.CacheFormData();
    missing.direct = "lateWidgetID";
    check(lateWidget.GetEditorID() == "lateWidgetID", "Widgets must still retry late direct IDs");

    entries.clear();
    RE::activeMap = &entries;
    POPULATE_FORM_FIXTURES
    CSEditor feature;
    feature.Prepass();
    check(savedLoads == 0, "Settings must wait for game data");
    feature.s_dataAvailable = true;
    feature.savedFilesExist = false;
    feature.Prepass();
    check(savedLoads == 0, "No saved settings retains lazy initialization");
    feature.savedFilesExist = true;
    const int beforeStartup = mapRequests, beforeVisits = mapVisits;
    feature.Prepass();
    check(savedLoads == 7 && feature.s_resourcesInitialized, "Every persistent widget collection loads without opening the editor");
    check(mapRequests == beforeStartup + 1 && mapVisits == beforeVisits + 9, "All nine collections share one map traversal");
    feature.Prepass();
    check(savedLoads == 7, "Initialized settings must not reload every frame");
    std::puts("PASS: lookup semantics, batch lifetime, one traversal, and automatic startup settings loading");
}
'''
        form_types = ["TESWeather", "BGSLightingTemplate", "TESImageSpace", "BGSVolumetricLighting",
                      "BGSShaderParticleGeometryData", "BGSLensFlare", "BGSReferenceEffect", "BGSArtObject", "TESEffectShader"]
        widget_types = ["WeatherWidget", "LightingTemplateWidget", "ImageSpaceWidget", "VolumetricLightingWidget",
                        "PrecipitationWidget", "LensFlareWidget", "ReferenceEffectWidget"]
        collections = ["weatherWidgets", "lightingTemplateWidgets", "imageSpaceWidgets", "volumetricLightingWidgets",
                       "precipitationWidgets", "lensFlareWidgets", "referenceEffectWidgets", "artObjectWidgets", "effectShaderWidgets"]
        replacements = {
            "FORM_HEADER": (ROOT / "src/Utils/Form.h").as_posix(),
            "FORMAT_SPID": braced(form, "std::string Util::FormatSpid("),
            "FORM_FILE_KEY": braced(form, "std::string Util::GetFormFileKey("),
            "FORM_INDEX": braced(form, "std::string Util::FormEditorIDIndex::Find("),
            "FORM_EDITOR_ID": braced(form, "std::string Util::GetFormEditorID("),
            "CACHE_FORM_DATA": braced(widget, "void CacheFormData("),
            "GET_EDITOR_ID": braced(widget, "virtual std::string GetEditorID("),
            "GET_SAVE_KEY": braced(widget, "std::string GetSaveKey("),
            "POPULATE_WIDGETS": braced(factory, "template <typename WidgetType, typename FormType>"),
            "POPULATE_SIMPLE_WIDGETS": braced(factory, "template <typename FormType>"),
            "EDITOR_SETUP": braced(editor, "void EditorWindow::SetupResources("),
            "SHOULD_PRELOAD": braced(feature, "bool CSEditor::ShouldPreloadEditorResources("),
            "ENSURE_DATA_LOADED": braced(feature, "void CSEditor::EnsureDataLoaded("),
            "PREPASS": braced(feature, "void CSEditor::Prepass("),
            "FORM_TYPES": "\n".join(f"struct {name} : TESForm {{}};" for name in form_types),
            "WIDGET_TYPES": "\n".join(f"struct {name} : Widget {{ explicit {name}(RE::TESForm* f) {{ form = f; }} }};" for name in widget_types),
            "WIDGET_COLLECTIONS": "\n".join(f"std::vector<std::unique_ptr<Widget>> {name};" for name in collections),
            "POPULATE_FORM_FIXTURES": "\n".join(
                f'RE::{name} form{i}; form{i}.localID = {i}; '
                f'RE::TESDataHandler::GetSingleton()->GetFormArray<RE::{name}>() = {{nullptr, &form{i}}}; '
                f'entries.push_back({{{{"alias{i}"}}, &form{i}}});'
                for i, name in enumerate(form_types, 1)),
        }
        for token, replacement in replacements.items():
            source = source.replace(token, replacement)
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)


if __name__ == "__main__":
    unittest.main()
