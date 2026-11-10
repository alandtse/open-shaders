#include "NativeFeatures.h"
#include "Globals.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "TruePBR.h"

// Backend selection is the ONLY automatic execution switch. Feature toggles
// are explicit user choices, never material/caster/light validity gates.
bool NativePBRFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }
bool NativeLLFFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }
bool NativeSCMFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }

namespace
{
    void DrawOwnership(const char* title, std::atomic<bool>& enabled, bool active)
    {
        bool selected = enabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox(title, &selected))
            enabled.store(selected, std::memory_order_relaxed);
        ImGui::TextDisabled("Rendering owner: %s", active ? "Native DX12" : "Legacy / inactive");
        ImGui::TextDisabled("Uses Skyrim's existing hooks; never installs a competing engine detour.");
    }
}

void NativePBRFeature::DrawSettings()
{
    DrawOwnership("Enable Native PBR", enabled, IsActive());
    ImGui::TextDisabled("Consumes TruePBR's live recipe registry in the native material worker.");
    ImGui::TextDisabled("Texture source and prepared material controls remain in DX12 Native Scene.");
}
void NativeLLFFeature::DrawSettings()
{
    DrawOwnership("Enable Native LLF", enabled, IsActive());
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto lights = globals::features::lightLimitFix.GetNativeLightFrame(frame);
    ImGui::Text("Captured current-frame LLF lights: %zu", lights ? lights->lights.size() : size_t{0});
    ImGui::TextDisabled("The existing DX12 shader batch supports up to 32 lights.");
}
void NativeSCMFeature::DrawSettings()
{
    DrawOwnership("Enable Native SCM", enabled, IsActive());
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto casters = ShadowCasterManager::GetNativeCasterFrame(frame);
    ImGui::Text("Tracked current-frame casters: %zu", casters ? casters->casters.size() : size_t{0});
    ImGui::TextDisabled("Prepared static casters belong to native TLAS; remaining casters use SCM tracking.");
    ImGui::TextDisabled("Dynamic geometry still requires an exact native shadow representation.");
}
void NativePBRFeature::LoadSettings(json& j) { enabled.store(j.value("Enabled", true), std::memory_order_relaxed); }
void NativeLLFFeature::LoadSettings(json& j) { enabled.store(j.value("Enabled", true), std::memory_order_relaxed); }
void NativeSCMFeature::LoadSettings(json& j) { enabled.store(j.value("Enabled", true), std::memory_order_relaxed); }
void NativePBRFeature::SaveSettings(json& j) { j = { { "Enabled", enabled.load(std::memory_order_relaxed) } }; }
void NativeLLFFeature::SaveSettings(json& j) { j = { { "Enabled", enabled.load(std::memory_order_relaxed) } }; }
void NativeSCMFeature::SaveSettings(json& j) { j = { { "Enabled", enabled.load(std::memory_order_relaxed) } }; }
json NativePBRFeature::GetDiagnostics()
{
    return { { "selected", enabled.load() }, { "native", IsActive() },
        { "recipeRevision", globals::features::truePBR.nativeRegistry.Revision() } };
}
json NativeLLFFeature::GetDiagnostics()
{
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto lights = globals::features::lightLimitFix.GetNativeLightFrame(frame);
    return { { "selected", enabled.load() }, { "native", IsActive() },
        { "observedLights", lights ? lights->lights.size() : size_t{0} } };
}
json NativeSCMFeature::GetDiagnostics()
{
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto casters = ShadowCasterManager::GetNativeCasterFrame(frame);
    return { { "selected", enabled.load() }, { "native", IsActive() },
        { "observedCasters", casters ? casters->casters.size() : size_t{0} },
        { "retiredPreparedStatic", casters ? casters->preparedStaticRetired : 0u } };
}
