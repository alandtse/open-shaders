import os
import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime
from test_scene_settings_runtime import braced


@unittest.skipUnless(os.name == "nt", "Uses the Windows build's native C++ dependencies")
class PostProcessingLifecycleTests(unittest.TestCase):
    def compile_and_run(self, source):
        dependencies = ROOT / "build/ALL/vcpkg_installed/x64-windows-static-md-release"
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source, imgui_root=dependencies)

    def test_resource_recreation_preserves_live_settings_and_applies_pending_changes(self):
        implementation = (ROOT / "src/Features/PostProcessing.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/PostProcessing.h").read_text(encoding="utf-8")
        methods = "\n".join(braced(implementation, declaration) for declaration in (
            "void PostProcessing::LoadSettings(",
            "void PostProcessing::ProcessSettings(",
            "void PostProcessing::SaveSettings(",
            "void PostProcessing::SaveActiveSettings(",
            "bool PostProcessing::ApplyPendingSettings(",
            "void PostProcessing::RestorePipelineDefaultEnablement(",
            "void PostProcessing::SetupResources(",
            "void PostProcessing::ClearShaderCache(",
            "void PostProcessing::CreatePipelineResources(",
            "void PostProcessing::SwapPipelineResources(",
            "bool PostProcessing::SelectPipelineResources(",
            "void PostProcessing::Reset(",
        ))
        source = r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace eastl { using std::make_unique; }
namespace logger {
template<class... T> void debug(T&&...) {}
template<class... T> void warn(T&&...) {}
template<class... T> void error(T&&...) {}
}
enum class SetupFailure { None, Texture, Sampler, Effect, FallbackTexture, FallbackEffect };
SetupFailure setupFailure = SetupFailure::None;
namespace DX {
struct com_exception : std::runtime_error { com_exception() : std::runtime_error("Injected Direct3D failure") {} };
void ThrowIfFailed(int result) { if (result) throw com_exception{}; }
}
struct ID3D11ShaderResourceView {};
struct D3D11_TEXTURE2D_DESC { int Format = 1, MipLevels = 1, BindFlags = 0, MiscFlags = 0; unsigned Width = 1920, Height = 1080; };
struct D3D11_RENDER_TARGET_VIEW_DESC { int Format, ViewDimension; struct { int MipSlice; } Texture2D; };
struct D3D11_SHADER_RESOURCE_VIEW_DESC { int Format, ViewDimension; struct { int MostDetailedMip, MipLevels; } Texture2D; };
constexpr int D3D11_BIND_RENDER_TARGET = 1, D3D11_BIND_SHADER_RESOURCE = 2;
constexpr int D3D11_RTV_DIMENSION_TEXTURE2D = 1, D3D11_SRV_DIMENSION_TEXTURE2D = 1;
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT = 2;
struct ID3D11Texture2D {
    D3D11_TEXTURE2D_DESC desc;
    void GetDesc(D3D11_TEXTURE2D_DESC* result) { *result = desc; }
};
using Resource = ID3D11Texture2D;
Resource resource;
int textureAllocations = 0;
struct Texture2D {
    D3D11_TEXTURE2D_DESC desc;
    Texture2D(D3D11_TEXTURE2D_DESC value, const char* name) : desc(value) {
        ++textureAllocations;
        if ((setupFailure == SetupFailure::Texture && std::string(name) == "PostProcessing::Output") ||
            (setupFailure == SetupFailure::FallbackTexture && value.Width == 1920 && std::string(name) == "PostProcessing::Linear Input"))
            throw DX::com_exception{};
    }
    void CreateRTV(D3D11_RENDER_TARGET_VIEW_DESC) {}
    void CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC) {}
};
struct Sampler {};
template<class T> struct View {
    T* value = nullptr;
    T* get() { return value; }
    T** put() { return &value; }
    explicit operator bool() const { return value != nullptr; }
    void operator=(std::nullptr_t) { value = nullptr; }
};
struct D3D11_SAMPLER_DESC { int Filter, AddressU, AddressV, AddressW, MaxAnisotropy; float MaxLOD; };
constexpr int D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0, D3D11_TEXTURE_ADDRESS_CLAMP = 0;
constexpr float D3D11_FLOAT32_MAX = 1e30f;
namespace globals::d3d {
struct Device {
    int CreateSamplerState(D3D11_SAMPLER_DESC*, Sampler**) { return setupFailure == SetupFailure::Sampler ? -1 : 0; }
} deviceStorage;
auto* device = &deviceStorage;
}
struct Feature {
    struct PostProcessingInput { unsigned width = 0, height = 0; Resource* texture = nullptr; ID3D11ShaderResourceView* srv = nullptr; } input;
    bool loaded = true;
    PostProcessingInput GetPostProcessingInput() const { return input; }
    static inline Feature* provider = nullptr;
    template<class Pred> static Feature* FindLoadedFeature(Pred pred) { return provider && provider->loaded && pred(provider) ? provider : nullptr; }
};
struct ConstantBuffer { ConstantBuffer(int, const char*) {} };
template<class T> int ConstantBufferDesc() { return 0; }
namespace RE::RENDER_TARGETS { enum { kMAIN, kMAIN_COPY }; }
struct Renderer {
    struct Target { Resource* texture = &resource; };
    struct Data { std::array<Target, 2> renderTargets; } data;
    Data& GetRuntimeData() { return data; }
} renderer;
namespace globals::game { auto* renderer = &::renderer; }
namespace Util {
template<class T> T* AsW32(T* value) { return value; }
void RequestTargetLockAPI() {}
void SetResourceName(Sampler*, const char*) {}
}
struct PostProcessing;
struct Effect {
    virtual ~Effect() = default;
    virtual std::string GetType() const = 0;
    bool enabled = true;
    PostProcessing* owner = nullptr;
    D3D11_TEXTURE2D_DESC outputDesc;
    int setupCalls = 0, shaderClears = 0;
    json settings = {{"currentTonemapper", "GT7"}, {"strength", 1.0}};
    bool IsAutoEnabled() const { return false; }
    void LoadSettings(json& value) { settings = value; }
    void SaveSettings(json& value) { value = settings; }
    void SetupResources();
    void ClearShaderCache() { ++shaderClears; }
    void Reset() {}
};
struct PostProcessing : Feature {
    PIPELINE_INDEX;
    std::array<std::shared_ptr<Effect>, static_cast<size_t>(FeaturePipelineIndex::COUNT)> pipeline;
    json settings = {{"DisableVanillaTonemapping", 1}}, pendingSettings;
    struct CinematicCamera {
        json settings = {{"fov", 75}};
        void LoadSettings(json& value) { settings = value; }
        void SaveSettings(json& value) { value = settings; }
    } cinematicCamera;
    struct Bokeh { void Setup() {} } bokehResources;
    struct CopyCB {};
    std::unique_ptr<Texture2D> texCopyMain, texCopyMainCopy, texInput, texOutput;
    View<Sampler> copySampler;
    View<Resource> fullscreenVS, copyPS;
    bool resourcesReady = false;
    Feature* inputProvider = nullptr;
    D3D11_TEXTURE2D_DESC pipelineTextureDesc;
    std::unique_ptr<ConstantBuffer> copyCB;
    ID3D11ShaderResourceView* postProcessingOutput = nullptr;
    bool isrefraction = false;
    void CompileCopyShaders() { fullscreenVS.value = copyPS.value = &resource; }
    void RestoreDefaultSettings() { std::abort(); }
    void RestorePipelineDefaultEnablement();
    void LoadSettings(json&);
    void ProcessSettings(json&);
    void SaveSettings(json&);
    void SaveActiveSettings(json&);
    bool ApplyPendingSettings();
    void SetupResources();
    void ClearShaderCache();
    void CreatePipelineResources(bool);
    void SwapPipelineResources();
    bool SelectPipelineResources(ID3D11Texture2D*);
    struct PipelineResources {
        decltype(pipeline) effects;
        std::unique_ptr<Texture2D> input, output;
        D3D11_TEXTURE2D_DESC desc{.Width = 0, .Height = 0};
    } alternatePipeline;
    void Reset();
};
void Effect::SetupResources() {
    ++setupCalls;
    outputDesc = owner->pipelineTextureDesc;
    if (setupFailure == SetupFailure::Effect || (setupFailure == SetupFailure::FallbackEffect && outputDesc.Width == 1920))
        throw DX::com_exception{};
}
template<PostProcessing::FeaturePipelineIndex Index> struct PipelineEffect : Effect {
    std::string GetType() const override { return std::to_string(static_cast<int>(Index)); }
};
EFFECT_TYPES
using HistogramAutoExposure = AutoExposure;
DEFAULT_ENABLEMENT
METHODS
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
    PostProcessing pp;
    const auto gradingIndex = static_cast<size_t>(PostProcessing::FeaturePipelineIndex::ColorGrading);
    const auto bloomIndex = static_cast<size_t>(PostProcessing::FeaturePipelineIndex::CODBloom);
    const auto grading = std::to_string(gradingIndex);
    const auto bloom = std::to_string(bloomIndex);
    json preset = {
        {grading, {{"enabled", true}, {"settings", {{"currentTonemapper", "AgX"}, {"strength", 2.5}}}}},
        {bloom, {{"enabled", false}, {"settings", {{"strength", 0.2}}}}},
        {"cinematic_camera", {{"fov", 90}}},
        {"ppsettings", {{"DisableVanillaTonemapping", 0}}}
    };
    pp.LoadSettings(preset);
    pp.SetupResources();
    check(pp.pendingSettings.empty(), "Initial setup consumes the pending preset");
    check(pp.pipeline[gradingIndex]->settings["currentTonemapper"] == "AgX", "Initial setup applies AgX");
    auto previousEffect = pp.pipeline[gradingIndex];
    pp.pipeline[gradingIndex]->settings["strength"] = 4.0;
    pp.pipeline[bloomIndex]->enabled = true;
    pp.cinematicCamera.settings["fov"] = 100;
    json before;
    pp.SaveSettings(before);
    ID3D11ShaderResourceView staleOutput;
    pp.postProcessingOutput = &staleOutput;
    pp.SetupResources();
    json after;
    pp.SaveSettings(after);
    check(before == after, "Resource recreation preserves live settings and effect enablement");
    check(previousEffect != pp.pipeline[gradingIndex], "Resource recreation replaces the effect objects");
    check(!pp.postProcessingOutput, "Resource recreation invalidates the previous scene SRV");
    json update = {{grading, {{"enabled", false}, {"settings", {{"currentTonemapper", "GT7"}}}}}};
    pp.LoadSettings(update);
    json queued;
    pp.SaveSettings(queued);
    check(queued == update, "Saving a queued update retains existing pending-settings semantics");
    pp.SetupResources();
    after = {};
    pp.SaveSettings(after);
    before[grading] = update[grading];
    check(before == after, "Pending changes override live settings without resetting unrelated effects");
    pp.postProcessingOutput = &staleOutput;
    pp.Reset();
    check(!pp.postProcessingOutput, "Frame reset invalidates the previous scene SRV");
    check(pp.pipelineTextureDesc.Width == 1920 && !pp.texOutput, "Default pipeline matches the engine size");
    Feature provider;
    provider.input.width = 2880;
    provider.input.height = 1620;
    Feature::provider = &provider;
    pp.SetupResources();
    check(pp.pipelineTextureDesc.Width == 2880 && pp.pipelineTextureDesc.Height == 1620,
          "Replacement resolution is available before the provider allocates its texture");
    check(pp.texInput->desc.Width == 2880 && pp.texOutput->desc.Width == 2880,
          "Input decoding and output conversion use the replacement resolution");
    check(pp.texCopyMain->desc.Width == 1920, "Engine copy fallback retains the engine resolution");
    const int allocationsBeforeSwitch = textureAllocations;
    auto fullInput = pp.texInput.get();
    auto fullBloom = pp.pipeline[bloomIndex];
    auto sharedGrading = pp.pipeline[gradingIndex];
    auto sharedExposure = pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::AutoExposure)];
    check(pp.SelectPipelineResources(&resource), "Fallback selects matching cached resources");
    check(pp.texInput->desc.Width == 1920 && pp.pipelineTextureDesc.Width == 1920 && !pp.texOutput,
          "Fallback input and effect resolution remain at engine size");
    check(pp.pipeline[gradingIndex] == sharedGrading &&
          pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::AutoExposure)] == sharedExposure,
          "Resolution switches preserve tonemapper shaders and exposure history");
    for (size_t i = 0; i < pp.pipeline.size(); ++i) {
        if (pp.pipeline[i] != pp.alternatePipeline.effects[i])
            check(pp.pipeline[i]->outputDesc.Width == 1920 && pp.pipeline[i]->outputDesc.Height == 1080,
                  "Fallback effect setup uses engine dimensions");
        check(pp.pipeline[i]->setupCalls == 1, "Shared effects are initialized once");
    }
    check(pp.pipeline[bloomIndex] != fullBloom && pp.pipeline[bloomIndex]->settings == fullBloom->settings,
          "Fallback has separate effect targets with the active settings");
    pp.pipeline[bloomIndex]->settings["strength"] = 0.6;
    auto fallbackInput = pp.texInput.get();
    check(pp.SelectPipelineResources(&resource) && pp.texInput.get() == fallbackInput,
          "Stable fallback reuses its resources");
    Resource display{{.Width = 2880, .Height = 1620}};
    check(pp.SelectPipelineResources(&display) && pp.texInput.get() == fullInput && pp.pipeline[bloomIndex] == fullBloom,
          "Returning to provider input reuses display-sized resources");
    check(pp.pipeline[bloomIndex]->settings["strength"] == 0.6, "Edits survive resolution switches");
    check(textureAllocations == allocationsBeforeSwitch, "Resolution switches do not allocate textures");
    pp.ClearShaderCache();
    for (size_t i = 0; i < pp.pipeline.size(); ++i) {
        check(pp.pipeline[i]->shaderClears == 1 && pp.alternatePipeline.effects[i]->shaderClears == 1,
              "Shader reload reaches both resolutions without recompiling shared effects twice");
    }
    for (auto failure : {SetupFailure::Texture, SetupFailure::Sampler, SetupFailure::Effect, SetupFailure::FallbackTexture, SetupFailure::FallbackEffect}) {
        for (bool queuedUpdate : {false, true}) {
            json expected;
            pp.SaveSettings(expected);
            if (queuedUpdate) {
                json change = {{bloom, {{"enabled", false}, {"settings", {{"strength", 0.75}}}}}};
                pp.LoadSettings(change);
                expected.update(change);
            }
            setupFailure = failure;
            pp.postProcessingOutput = &staleOutput;
            pp.SetupResources();
            check(!pp.resourcesReady && !pp.postProcessingOutput, "Failed setup disables the pipeline and its published output");
            check(!pp.texInput && !pp.texOutput && !pp.copyCB && !pp.copySampler && !pp.fullscreenVS && !pp.copyPS,
                  "Failed setup releases partial resources");
            for (auto& effect : pp.pipeline)
                check(!effect, "Failed setup releases partial effects");
            for (auto& effect : pp.alternatePipeline.effects)
                check(!effect, "Failed setup releases cached effects");
            check(!pp.alternatePipeline.input && !pp.alternatePipeline.output, "Failed setup releases cached textures");
            check(!pp.ApplyPendingSettings(), "Unavailable effects cannot consume saved settings");
            json saved;
            pp.SaveSettings(saved);
            check(saved == expected, "Failed setup preserves active settings and queued overrides");
            pp.SetupResources();
            pp.SaveSettings(saved);
            check(saved == expected, "Repeated setup failure preserves the recovery snapshot");
            setupFailure = SetupFailure::None;
            pp.SetupResources();
            saved = {};
            pp.SaveSettings(saved);
            check(pp.resourcesReady && pp.pendingSettings.empty() && saved == expected,
                  "Successful retry restores settings and reenables processing");
        }
    }
    provider.loaded = false;
    pp.SetupResources();
    check(pp.pipelineTextureDesc.Width == 1920 && !pp.texOutput && !pp.inputProvider,
          "An unavailable provider restores engine resolution and drops replacement resources");
}
'''
        index = braced(header, "enum class FeaturePipelineIndex")
        effect_names = [line.strip().rstrip(",") for line in index.splitlines()[2:-1]]
        effects = "\n".join(
            f"using {name} = PipelineEffect<PostProcessing::FeaturePipelineIndex::{name}>;"
            for name in effect_names if name and name != "COUNT"
        )
        source = source.replace("PIPELINE_INDEX", index).replace("EFFECT_TYPES", effects)
        source = source.replace("DEFAULT_ENABLEMENT", braced(implementation, "constexpr bool IsPipelineFeatureEnabledByDefault("))
        self.compile_and_run(source.replace("METHODS", methods))

    def test_tonemap_consumes_processed_scene_and_restores_engine_bindings(self):
        implementation = (ROOT / "src/Features/Upscaling/PerfMode/PostIntercept.cpp").read_text(encoding="utf-8")
        feature = (ROOT / "src/Feature.h").read_text(encoding="utf-8")
        source = r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CS_GPU_PASS(name)
struct ID3D11ShaderResourceView {};
struct DepthView {};
template<class T> struct View {
    T* value = nullptr;
    T* get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
namespace Util {
template<class T> T* AsW32(T* value) { return value; }
template<class T> T* AsReal(T* value) { return value; }
}
namespace RE {
struct BSTriShape {}; struct ImageSpaceEffectParam {};
namespace RENDER_TARGETS { enum { kMAIN, kMAIN_COPY, kTOTAL }; }
namespace RENDER_TARGETS_DEPTHSTENCIL { enum { kMAIN }; }
namespace BSGraphics {
struct Renderer {
    struct Target { ID3D11ShaderResourceView* SRV = nullptr; };
    struct Data { std::array<Target, 2> renderTargets; } data;
    struct Depth { DepthView* views[8]{}; DepthView* readOnlyViews[8]{}; };
    struct DepthData { std::array<Depth, 1> depthStencils; } depthData;
    Data& GetRuntimeData() { return data; }
    DepthData& GetDepthStencilData() { return depthData; }
    static Renderer* GetSingleton() { static Renderer renderer; return &renderer; }
};
}
}
struct Feature {
    bool loaded = true;
    ID3D11ShaderResourceView* output = nullptr;
    ID3D11ShaderResourceView* GetPostProcessingOutput() const { return output; }
    static auto& GetFeatureList() { static std::vector<Feature*> features; return features; }
    template<class Pred> FIND_FEATURE
};
struct PerfMode {
    bool hookActive = true;
    View<ID3D11ShaderResourceView> testTextureSRV;
    View<DepthView> fakeDSV;
    ID3D11ShaderResourceView *savedKMainSRV = nullptr, *savedKMainCopySRV = nullptr;
    DepthView* savedKMainViews[8]{};
    DepthView* savedKMainReadOnlyViews[8]{};
    void MaybeBlitMenuBG(int) {}
    template<class Hook> static void RenderTonemapWithSwap(void*, RE::BSTriShape*, RE::ImageSpaceEffectParam*);
};
namespace globals {
struct State { bool worldRenderedThisFrame = true; bool IsMainOrLoadingMenuOpen() { return false; } } stateStorage;
auto* state = &stateStorage;
namespace features { struct Upscaling { PerfMode perfMode; } upscaling; }
}
template<class Hook> TONEMAP
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
ID3D11ShaderResourceView* expected;
struct Hook {
    static void func(void*, RE::BSTriShape*, RE::ImageSpaceEffectParam*) {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        check(renderer->data.renderTargets[0].SRV == expected, "Tonemap receives the completed scene");
        check(renderer->data.renderTargets[1].SRV == expected, "Refraction uses the same completed scene");
        check(renderer->depthData.depthStencils[0].views[0] == globals::features::upscaling.perfMode.fakeDSV.get(),
              "Display-resolution depth remains active during tonemap");
    }
};
int main() {
    ID3D11ShaderResourceView raw, main, mainCopy, processed;
    DepthView engineDepth, displayDepth;
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    renderer->data.renderTargets = {{{&main}, {&mainCopy}}};
    renderer->depthData.depthStencils[0].views[0] = &engineDepth;
    renderer->depthData.depthStencils[0].readOnlyViews[0] = &engineDepth;
    auto& perfMode = globals::features::upscaling.perfMode;
    perfMode.testTextureSRV.value = &raw;
    perfMode.fakeDSV.value = &displayDepth;
    Feature inactive, producer;
    Feature::GetFeatureList() = {&inactive, &producer};
    for (bool loaded : {false, true}) for (bool hasOutput : {false, true}) {
        producer.loaded = loaded;
        producer.output = hasOutput ? &processed : nullptr;
        expected = loaded && hasOutput ? &processed : &raw;
        PerfMode::RenderTonemapWithSwap<Hook>(nullptr, nullptr, nullptr);
        check(renderer->data.renderTargets[0].SRV == &main && renderer->data.renderTargets[1].SRV == &mainCopy,
              "Tonemap restores both engine SRVs");
        check(renderer->depthData.depthStencils[0].views[0] == &engineDepth &&
              renderer->depthData.depthStencils[0].readOnlyViews[0] == &engineDepth,
              "Tonemap restores writable and read-only depth views");
        check(!renderer->depthData.depthStencils[0].views[1], "Absent depth views stay absent");
    }
}
'''
        source = source.replace("FIND_FEATURE", braced(feature, "static Feature* FindLoadedFeature("))
        source = source.replace("TONEMAP", braced(implementation, "void PerfMode::RenderTonemapWithSwap("))
        self.compile_and_run(source)

    def test_pipeline_preserves_input_resolution_without_resampling(self):
        implementation = (ROOT / "src/Features/PostProcessing.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/PostProcessing.h").read_text(encoding="utf-8")
        methods = "\n".join(braced(implementation, declaration) for declaration in (
            "void PostProcessing::PreProcess(",
            "void PostProcessing::BeginLinearProcessing(",
            "void PostProcessing::DrawCopy(",
            "void PostProcessing::CopyToRenderTarget(",
            "void PostProcessing::SwapPipelineResources(",
            "bool PostProcessing::SelectPipelineResources(",
            "bool PostProcessing::WantsTonemapOwnership(",
        ))
        source = r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#define CS_GPU_PASS(name)
using json = int;
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct D3D11_TEXTURE2D_DESC { unsigned Width = 0, Height = 0, Format = 1; };
struct ID3D11Texture2D {
    D3D11_TEXTURE2D_DESC desc;
    void GetDesc(D3D11_TEXTURE2D_DESC* result) { *result = desc; }
};
struct ID3D11ShaderResourceView { ID3D11Texture2D* texture; };
struct ID3D11RenderTargetView { ID3D11Texture2D* texture; };
struct ID3D11SamplerState {} sampler;
struct ID3D11Buffer {} buffer;
template<class T> struct View {
    T* value = nullptr;
    T* get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
struct Texture2D {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D storage;
    ID3D11ShaderResourceView srvStorage;
    ID3D11RenderTargetView rtvStorage;
    View<ID3D11Texture2D> resource;
    View<ID3D11ShaderResourceView> srv;
    View<ID3D11RenderTargetView> rtv;
    Texture2D(unsigned width, unsigned height) : desc{width, height}, storage{desc},
        srvStorage{&storage}, rtvStorage{&storage}, resource{&storage}, srv{&srvStorage}, rtv{&rtvStorage} {}
};
namespace Util {
template<class T> T* AsReal(T* value) { return value; }
template<class T> T* AsW32(T* value) { return value; }
}
namespace RE {
using RENDER_TARGET = int;
namespace RENDER_TARGETS { enum { kMAIN, kMAIN_COPY }; }
namespace BSGraphics {
struct ShaderFlags { enum { DIRTY_RENDERTARGET }; };
struct RenderTargetData { ID3D11Texture2D* texture; ID3D11ShaderResourceView* SRV; };
}
}
struct State {
    enum class TonemapOwner { kPostProcessing, kEffects11, kVanilla } owner = TonemapOwner::kPostProcessing;
    bool menu = false;
    bool IsMainOrLoadingMenuOpen() { return menu; }
    TonemapOwner GetTonemapOwner() { return owner; }
    void SetOutputRenderTarget(int) {}
};
unsigned resample = 0;
int copies = 0, rasterDraws = 0, resamples = 0;
struct ConstantBuffer {
    template<class T> void Update(const T& data) { resample = data.resample; }
    ID3D11Buffer* CB() { return &buffer; }
};
namespace globals {
State storage; auto* state = &storage;
namespace game {
struct Renderer {
    struct Data { std::array<RE::BSGraphics::RenderTargetData, 2> renderTargets; } data;
    Data& GetRuntimeData() { return data; }
} rendererStorage;
auto* renderer = &rendererStorage;
struct Flags { void set(int) {} } flags;
auto* stateUpdateFlags = &flags;
}
namespace d3d {
struct Context {
    ID3D11ShaderResourceView* input = nullptr;
    ID3D11RenderTargetView* output = nullptr;
    void OMSetRenderTargets(int, void*, void*) {}
    void PSSetConstantBuffers(int, int, ID3D11Buffer**) {}
    void PSSetSamplers(int, int, ID3D11SamplerState**) {}
    void PSSetShaderResources(int, int, ID3D11ShaderResourceView** srv) { input = *srv; }
    void CopySubresourceRegion(ID3D11Texture2D* dst, int, int, int, int, ID3D11Texture2D* src, int, void*) {
        check(dst->desc.Width == src->desc.Width && dst->desc.Height == src->desc.Height,
              "Raw copies require matching dimensions");
        ++copies;
    }
} storage;
auto* context = &storage;
}
namespace features {
struct Upscaling { bool loaded = true; } upscaling;
struct LinearLighting {
    struct Settings { bool enableACEScg = false; } settings;
    bool active = true;
    bool IsLinearLightingActive() const { return active; }
} linearLighting;
}
}
namespace PostProcessingRaster {
struct RasterPass {
    globals::d3d::Context* context;
    RasterPass(globals::d3d::Context* value) : context(value) {}
    void SetTargets(std::initializer_list<ID3D11RenderTargetView*> targets, float width, float height) {
        context->output = *targets.begin();
        check(width == context->output->texture->desc.Width && height == context->output->texture->desc.Height,
              "Copy viewport matches its target");
    }
    void SetShaders(int*, int*) {}
    void Draw() {
        const auto& src = context->input->texture->desc;
        const auto& dst = context->output->texture->desc;
        check(bool(resample) == (src.Width != dst.Width || src.Height != dst.Height),
              "Copy constants reflect actual source and target dimensions");
        ++rasterDraws;
        resamples += resample != 0;
    }
};
}
struct PostProcessFeature {
    enum class Gamut { Rec709, ACEScg, Rec2020 };
    struct TextureInfo { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; Gamut gamut = Gamut::Rec709; };
    bool enabled = true;
    int settings = 1;
    std::unique_ptr<Texture2D> output;
    bool IsAutoEnabled() { return false; }
    void UpdateAutoEnabled() {}
    bool IsActive() { return enabled; }
    bool DrawAfterColorGrading() { return false; }
    bool DisableInMainLoadingMenu() { return false; }
    bool DrawBeforeUpscaling() { return false; }
    void SaveSettings(json& value) { value = settings; }
    void LoadSettings(json value) { settings = value; }
    void Reset() {}
};
struct ColorGrading : PostProcessFeature {
    struct Settings { bool enableTonemap = true; } settings;
    Gamut GetDisplayGamut() const { return Gamut::Rec709; }
    bool IsReadyForTonemapping() const { return true; }
};
struct Feature {
    struct PostProcessingInput { ID3D11Texture2D* texture = nullptr; ID3D11ShaderResourceView* srv = nullptr; } input;
    bool loaded = true;
    PostProcessingInput GetPostProcessingInput() { return input; }
};
struct PostProcessing : Feature {
    using Gamut = PostProcessFeature::Gamut;
    enum class FeaturePipelineIndex { ColorGrading };
    struct CopyCB { Gamut inputGamut = Gamut::Rec709, outputGamut = Gamut::Rec709; float gamma = 1; unsigned resample = 0; };
    static constexpr float kLegacySceneGamma = 1.6f;
    bool bypass = false, isrefraction = false, resourcesReady = true;
    int shader = 0;
    View<int> fullscreenVS{&shader}, copyPS{&shader};
    View<ID3D11SamplerState> copySampler{&sampler};
    std::unique_ptr<ConstantBuffer> copyCB = std::make_unique<ConstantBuffer>();
    PIPELINE_READY
    struct Settings { int DisableVanillaTonemapping = 1; } settings;
    bool WantsTonemapOwnership() const;
    Feature* inputProvider = nullptr;
    ID3D11ShaderResourceView* postProcessingOutput = nullptr;
    std::unique_ptr<Texture2D> texCopyMain, texCopyMainCopy, texInput, texOutput;
    std::array<std::shared_ptr<PostProcessFeature>, 2> pipeline;
    D3D11_TEXTURE2D_DESC pipelineTextureDesc{2880, 1620};
    struct PipelineResources {
        decltype(pipeline) effects;
        std::unique_ptr<Texture2D> input, output;
        D3D11_TEXTURE2D_DESC desc{1920, 1080};
    } alternatePipeline;
    ColorGrading grading;
    int draws = 0;
    bool IsTonemapOwnedByEffects11() { return globals::state->owner == State::TonemapOwner::kEffects11; }
    template<class T> const T* GetPipelineFeature(FeaturePipelineIndex) const { return &grading; }
    void DrawFeature(PostProcessFeature& effect, PostProcessFeature::TextureInfo& input) {
        check(effect.enabled, "Disabled effects never draw");
        check(effect.output->desc.Width == input.tex->desc.Width && effect.output->desc.Height == input.tex->desc.Height,
              "Effects consume and produce the same resolution");
        check(effect.output->desc.Width == pipelineTextureDesc.Width, "Effects match the active pipeline descriptor");
        input.tex = effect.output->resource.get();
        input.srv = effect.output->srv.get();
        ++draws;
    }
    void BeginLinearProcessing(PostProcessFeature::TextureInfo&, bool = true);
    void CopyToRenderTarget(RE::BSGraphics::RenderTargetData&, Texture2D*, ID3D11Texture2D*, ID3D11ShaderResourceView*, const CopyCB&);
    void DrawCopy(Texture2D&, ID3D11Texture2D*, ID3D11ShaderResourceView*, CopyCB);
    bool SelectPipelineResources(ID3D11Texture2D*);
    void SwapPipelineResources();
    void PreProcess(int, int);
};
METHODS
int main() {
    Texture2D render(1920, 1080), renderCopy(1920, 1080), display(2880, 1620);
    globals::game::renderer->data.renderTargets = {{{render.resource.get(), render.srv.get()}, {renderCopy.resource.get(), renderCopy.srv.get()}}};
    Feature provider;
    PostProcessing pp;
    pp.inputProvider = &provider;
    pp.texInput = std::make_unique<Texture2D>(2880, 1620);
    pp.texOutput = std::make_unique<Texture2D>(2880, 1620);
    pp.alternatePipeline.input = std::make_unique<Texture2D>(1920, 1080);
    pp.texCopyMain = std::make_unique<Texture2D>(1920, 1080);
    for (auto* effects : {&pp.pipeline, &pp.alternatePipeline.effects}) {
        const bool full = effects == &pp.pipeline;
        for (auto& effect : *effects) {
            effect = std::make_shared<PostProcessFeature>();
            effect->output = std::make_unique<Texture2D>(full ? 2880 : 1920, full ? 1620 : 1080);
        }
        (*effects)[1]->enabled = false;
    }
    for (bool providerActive : {true, false, false, true})
    for (bool linear : {true, false})
    for (bool menu : {false, true})
    for (bool refraction : {false, true})
    for (auto owner : {State::TonemapOwner::kPostProcessing, State::TonemapOwner::kVanilla}) {
        provider.input = providerActive ? Feature::PostProcessingInput{display.resource.get(), display.srv.get()} : Feature::PostProcessingInput{};
        globals::features::linearLighting.active = linear;
        globals::state->owner = owner;
        globals::state->menu = menu;
        pp.isrefraction = refraction;
        pp.draws = copies = rasterDraws = resamples = 0;
        pp.PreProcess(refraction ? RE::RENDER_TARGETS::kMAIN_COPY : RE::RENDER_TARGETS::kMAIN, 0);
        check(resamples == 0, "Neither provider nor fallback processing resamples the scene");
        check(pp.draws == 1, "Only the enabled effect draws");
        check(pp.pipelineTextureDesc.Width == (providerActive ? 2880u : 1920u), "Pipeline follows the actual input");
        if (providerActive) {
            check(pp.postProcessingOutput && pp.postProcessingOutput->texture->desc.Width == 2880,
                  "Provider publishes full-resolution output");
            check(copies == 0, "Provider output avoids engine target copies");
        } else {
            check(!pp.postProcessingOutput && copies == 2, "Fallback uses only engine-sized targets");
        }
    }
    pp.bypass = true;
    pp.postProcessingOutput = display.srv.get();
    pp.PreProcess(0, 0);
    check(!pp.postProcessingOutput, "Bypass cannot publish stale output");
    pp.bypass = false;
    const auto drawsBeforeFailure = pp.draws;
    for (bool shadersReady : {false, true}) {
        pp.resourcesReady = false;
        pp.fullscreenVS.value = pp.copyPS.value = shadersReady ? &pp.shader : nullptr;
        pp.postProcessingOutput = display.srv.get();
        pp.PreProcess(0, 0);
        check(!pp.postProcessingOutput && pp.draws == drawsBeforeFailure,
              "Shader recompilation cannot enable a pipeline with failed resource setup");
        check(!pp.WantsTonemapOwnership(), "Failed setup leaves tonemapping to the game pipeline");
    }
}
'''
        source = source.replace("PIPELINE_READY", braced(header, "bool IsPipelineReady("))
        self.compile_and_run(source.replace("METHODS", methods))


if __name__ == "__main__":
    unittest.main()
