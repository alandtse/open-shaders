import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


class ForceWeatherTests(unittest.TestCase):
    def test_cloud_passes_and_weather_model_refresh(self):
        game = (ROOT / "src/Utils/Game.cpp").read_text(encoding="utf-8")
        source = r'''
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>
void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
namespace RE {
struct TESWeather {};
struct Property { virtual ~Property() = default; };
struct BSSkyShaderProperty : Property {
    std::int32_t lastRenderPassState = 0;
    float blend = 0.5f;
    bool blendPass = true;
    void DoClearRenderPasses() {
        lastRenderPassState = (std::numeric_limits<std::int32_t>::max)();
    }
    bool GetRenderPasses() {
        if (lastRenderPassState == (std::numeric_limits<std::int32_t>::max)()) {
            blendPass = blend > 0;
            lastRenderPassState = 0;
        }
        return blendPass;
    }
};
struct Geometry {
    struct Data { std::shared_ptr<Property> shaderProperty; } data;
    Data& GetGeometryRuntimeData() { return data; }
};
struct Clouds { std::array<std::shared_ptr<Geometry>, 32> clouds{}; };
struct NiNode {
    std::shared_ptr<NiNode> child;
    int detaches = 0;
    void DetachChild(NiNode* node) {
        require(child.get() == node, "Detach the outgoing model from its sky root");
        child.reset();
        ++detaches;
    }
};
struct ModelDBHandle {
    struct U_Entry { int references = 1; };
    U_Entry* entry = nullptr;
    U_Entry* get() const { return entry; }
    explicit operator bool() const { return entry != nullptr; }
};
struct Sky {
    std::shared_ptr<NiNode> root = std::make_shared<NiNode>(), auroraRoot;
    ModelDBHandle auroraModel;
    Clouds* clouds = nullptr;
    TESWeather* currentWeather = nullptr;
    TESWeather* overrideWeather = nullptr;
    TESWeather* defaultWeather = nullptr;
    void ForceWeather(TESWeather* weather, bool override) {
        currentWeather = weather;
        overrideWeather = override ? weather : nullptr;
        defaultWeather = override ? nullptr : weather;
        if (clouds) for (const auto& cloud : clouds->clouds) if (cloud) {
            if (auto* p = dynamic_cast<BSSkyShaderProperty*>(cloud->data.shaderProperty.get()))
                p->blend = 0;
        }
    }
};
}
template<class T, class U> T skyrim_cast(U* value) { return dynamic_cast<T>(value); }
namespace REL {
struct Module { static inline bool ae = false; static bool IsAE() { return ae; } };
struct ID { explicit ID(int) {} };
struct VariantID { VariantID(int, int, int) {} };
int released = 0, reset = 0;
template<class F> struct Relocation {
    template<class T> explicit Relocation(T) {}
    template<class... Args> auto operator()(Args... args) const {
        if constexpr (std::is_same_v<F, void (*)(RE::ModelDBHandle::U_Entry*)>) {
            auto release = [](RE::ModelDBHandle::U_Entry* entry) { --entry->references; ++released; };
            return release(args...);
        } else {
            auto clear = [](RE::ModelDBHandle* handle, RE::ModelDBHandle::U_Entry* entry) {
                require(entry == nullptr, "Reset clears the model request");
                --handle->entry->references;
                handle->entry = entry;
                ++reset;
                return handle;
            };
            return clear(args...);
        }
    }
};
}
RESET_MODEL_HANDLE
namespace Util {
FORCE_WEATHER
}
int main() {
    for (bool ae : {false, true}) {
        REL::Module::ae = ae;
        RE::Sky sky;
        RE::TESWeather first, second;
        RE::Clouds clouds;
        sky.clouds = &clouds;
        for (int index : {0, 31}) {
            clouds.clouds[index] = std::make_shared<RE::Geometry>();
            clouds.clouds[index]->data.shaderProperty = std::make_shared<RE::BSSkyShaderProperty>();
        }
        clouds.clouds[1] = std::make_shared<RE::Geometry>();
        clouds.clouds[2] = std::make_shared<RE::Geometry>();
        clouds.clouds[2]->data.shaderProperty = std::make_shared<RE::Property>();
        sky.currentWeather = &first;
        sky.auroraRoot = sky.root->child = std::make_shared<RE::NiNode>();
        std::weak_ptr<RE::NiNode> oldRoot = sky.auroraRoot;
        RE::ModelDBHandle::U_Entry oldRequest;
        sky.auroraModel.entry = &oldRequest;
        Util::ForceWeather(&sky, &second, true);
        require(sky.currentWeather == &second && sky.overrideWeather == &second && !sky.defaultWeather,
                "Force preserves the override contract");
        require(oldRoot.expired() && sky.root->detaches == 1 && !sky.auroraRoot,
                "Outgoing aurora/sky model no longer blocks the incoming model");
        require(!sky.auroraModel && oldRequest.references == 0, "Release the old model request exactly once");
        for (int index : {0, 31}) {
            auto* p = static_cast<RE::BSSkyShaderProperty*>(clouds.clouds[index]->data.shaderProperty.get());
            require(!p->GetRenderPasses(), "Forced clouds rebuild as single-texture passes");
        }
        RE::ModelDBHandle::U_Entry pendingRequest;
        sky.auroraModel.entry = &pendingRequest;
        sky.clouds = nullptr;
        Util::ForceWeather(&sky, &first, false);
        require(!sky.auroraModel && pendingRequest.references == 0 && sky.root->detaches == 1,
                "Rapid weather changes cancel a pending model before it attaches");
        require(sky.defaultWeather == &first && !sky.overrideWeather, "Preserve non-override weather selection");
        Util::ForceWeather(&sky, &first, false);
        require(pendingRequest.references == 0, "Repeated force cannot release a request twice");
        sky.root.reset();
        Util::ForceWeather(&sky, nullptr, false);
        require(!sky.currentWeather, "Null weather retains the native reset behavior");
        Util::ForceWeather(nullptr, &first, true);
    }
    require(REL::released == 2 && REL::reset == 2, "Both native model-release paths handle attached and pending models");
}
'''
        source = source.replace("RESET_MODEL_HANDLE", braced(game, "void ResetModelHandle("))
        source = source.replace("FORCE_WEATHER", braced(game, "void ForceWeather("))
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)


if __name__ == "__main__":
    unittest.main()
