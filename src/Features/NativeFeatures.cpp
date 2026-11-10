#include "NativeFeatures.h"
#include "Globals.h"
#include "Features/DX12NativeScene/DX12NativeScene.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ShadowCasterManager.h"
#include "TruePBR.h"
#include "State.h"
#include "Features/NativeLighting/NativeLighting.h"
#include "Utils/PointLightFlags.h"
#include <algorithm>
#include <cmath>
#include <limits>

// Backend selection is the ONLY automatic execution switch. Feature toggles
// are explicit user choices, never material/caster/light validity gates.
bool NativePBRFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }
bool NativeLLFFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }
bool NativeSCMFeature::IsActive() const { return loaded && enabled.load(std::memory_order_relaxed) && globals::features::dx12NativeScene.OwnsNativeFeatures(); }

bool NativeSCMFeature::OwnsStaticForLight(const RE::BSShadowLight* source, int32_t slot) const
{
    if (!source || slot < 0 || !IsActive() || !globals::features::nativeLLF.IsActive() ||
        !globals::game::graphicsState)
        return false;

    // SCM shadow rendering is scheduled before LLF publishes the *current*
    // frame's raster light buffer. Use the last complete LLF generation, and
    // match BOTH slot and world-space light identity: a reassigned slot must
    // never inherit another light's native shadow ownership.
    const auto frame = globals::game::graphicsState->GetFrameCount();
    const auto published = globals::features::nativeLLF.GetRecentLightFrame(frame);
    const auto* niLight = source->light.get();
    if (!published || !niLight) return false;

    const auto& nativeScene = globals::features::dx12NativeScene;
    const auto prepared = nativeScene.GetPublishedScene();
    if (!prepared) return false;
    // NativeSCM selection must reference the prepared-first unified indices,
    // not the old, unrelated raw LLF array indices.
    const auto unified = globals::features::nativeLLF.BuildUnifiedLights(
        prepared->cell->nativeLights, frame, true);
    const auto& direct = globals::features::nativeLighting;
    for (uint32_t index = 0; index < unified.count; ++index) {
        const auto& identity = unified.source[index];
        if (!identity.valid ||
            (identity.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) == 0 ||
            identity.shadowSlot != static_cast<uint32_t>(slot)) continue;

        const bool maskSelected = nativeScene.settings.ShadowMask &&
            (nativeScene.settings.ShadowAllLights || nativeScene.settings.ShadowLightIndex == index);
        const bool directSelected = direct.SelectsLightForNativeShadows(index);
        if (!maskSelected && !directSelected) continue;

        const auto& p = niLight->world.translate;
        const float dx = p.x - identity.position.x, dy = p.y - identity.position.y, dz = p.z - identity.position.z;
        const float tolerance = std::clamp(identity.radius * 0.01f, 4.0f, 32.0f);
        if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
            dx * dx + dy * dy + dz * dz <= tolerance * tolerance)
            return true;
    }
    return false;
}

void NativeLLFFeature::ClearCapturedLights()
{
    std::scoped_lock lock(lightMutex);
    lightFrame.reset();
}

void NativeLLFFeature::OnSceneTransitionReset(bool a_open)
{
    transitionOpen.store(a_open, std::memory_order_release);
    ClearCapturedLights();
}

void NativeLLFFeature::PublishLights(const LightLimitFix::LightData* data, uint32_t count, const float3& eye, uint32_t frame)
{
    if (!IsActive() || transitionOpen.load(std::memory_order_acquire)) {
        ClearCapturedLights();
        return;
    }
    auto generation = std::make_shared<LightFrame>();
    generation->frame = frame;
    generation->lights.reserve(count);
    for (uint32_t i = 0; data && i < count; ++i) {
        const auto& source = data[i];
        if (source.lightFlags.any(LightLimitFix::LightFlags::Disabled)) continue;
        const auto& p = source.positionWS[0].data;
        const Light light{
            { p.x + eye.x, p.y + eye.y, p.z + eye.z },
            { source.color.x * source.fade, source.color.y * source.fade, source.color.z * source.fade },
            source.radius, std::sqrt(std::max(source.sizeBias, 0.0f)), source.fadeZone,
            static_cast<uint32_t>(source.lightFlags.underlying()), source.shadowMapIndex
        };
        if (!std::isfinite(light.position.x) || !std::isfinite(light.position.y) || !std::isfinite(light.position.z) ||
            !std::isfinite(light.radiance.x) || !std::isfinite(light.radiance.y) || !std::isfinite(light.radiance.z) ||
            !std::isfinite(light.radius) || light.radius <= 0.0f) continue;
        generation->lights.push_back(light);
    }
    std::scoped_lock lock(lightMutex);
    // A loading transition may have started while the new snapshot was built.
    // Never republish the old cell after its clear operation.
    if (transitionOpen.load(std::memory_order_acquire)) lightFrame.reset();
    else lightFrame = std::move(generation);
}
std::shared_ptr<const NativeLLFFeature::LightFrame> NativeLLFFeature::GetLightFrame(uint32_t frame) const
{
    std::scoped_lock lock(lightMutex);
    return !transitionOpen.load(std::memory_order_acquire) && lightFrame && lightFrame->frame == frame ? lightFrame : nullptr;
}
std::shared_ptr<const NativeLLFFeature::LightFrame> NativeLLFFeature::GetRecentLightFrame(uint32_t frame) const
{
    std::scoped_lock lock(lightMutex);
    return !transitionOpen.load(std::memory_order_acquire) && lightFrame && uint32_t(frame - lightFrame->frame) <= 1u ? lightFrame : nullptr;
}


NativeLLFFeature::UnifiedLights NativeLLFFeature::BuildUnifiedLights(
    std::span<const DX12Native::PreparedPointLight> prepared, uint32_t frame, bool allowRecent) const
{
    UnifiedLights result{};
    result.prepared = static_cast<uint32_t>(prepared.size());
    result.count = static_cast<uint32_t>(std::min(prepared.size(), result.lights.size()));
    std::copy_n(prepared.begin(), result.count, result.lights.begin());
    result.overflow = result.prepared - result.count;
    // No live LLF data must NEVER erase the authoritative prepared lighting.
    if (!IsActive()) return result;
    const auto published = allowRecent ? GetRecentLightFrame(frame) : GetLightFrame(frame);
    if (!published) return result;
    result.observed = static_cast<uint32_t>(published->lights.size());

    const float units = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
    const float radiusSquared = units * units;
    const float gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
    const bool contribute = contributeUnmatched.load(std::memory_order_relaxed) && gain > 0.0f;
    std::array<float, kMaxUnifiedLights> identityDistances{};
    identityDistances.fill(std::numeric_limits<float>::infinity());
    const auto distanceSquared = [](const float3& a, const float3& b) {
        const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return x * x + y * y + z * z;
    };
    for (const auto& live : published->lights) {
        uint32_t closest = UINT32_MAX;
        float bestSquared = std::numeric_limits<float>::infinity();
        // First match all supported prepared sources, regardless of whether
        // LLF augmentation is enabled. Unsupported prepared placeholders must
        // never suppress valid live illumination.
        for (uint32_t i = 0; i < std::min(result.prepared, result.count); ++i) {
            if (!result.lights[i].supported) continue;
            const float dist2 = distanceSquared(live.position, result.lights[i].position);
            if (dist2 <= radiusSquared && dist2 < bestSquared) {
                closest = i; bestSquared = dist2;
            }
        }
        // Only if no prepared match exists, deduplicate additional LLF sources.
        for (uint32_t i = std::min(result.prepared, result.count); closest == UINT32_MAX && i < result.count; ++i) {
            const float dist2 = distanceSquared(live.position, result.lights[i].position);
            if (dist2 <= radiusSquared && dist2 < bestSquared) {
                closest = i; bestSquared = dist2;
            }
        }
        if (closest != UINT32_MAX) {
            if (closest < result.prepared) ++result.matched;
            else ++result.clusteredLive;
            const auto& previous = result.source[closest];
            const bool shadow = (live.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) != 0;
            const bool priorShadow = previous.valid &&
                (previous.flags & static_cast<uint32_t>(PointLightFlags::Flags::Shadow)) != 0;
            if (!previous.valid || (shadow && !priorShadow) ||
                (shadow == priorShadow && bestSquared < identityDistances[closest])) {
                result.source[closest] = { live.position, live.radius, live.flags, live.shadowSlot, true };
                identityDistances[closest] = bestSquared;
            }
            continue;
        }
        if (!contribute) { ++result.omitted; continue; }
        if (result.count == result.lights.size()) { ++result.overflow; continue; }
        const uint32_t target = result.count++;
        result.lights[target] = { live.position,
            { live.radiance.x * gain, live.radiance.y * gain, live.radiance.z * gain },
            live.radius, live.sourceRadius, true };
        result.source[target] = { live.position, live.radius, live.flags, live.shadowSlot, true };
        identityDistances[target] = 0.0f;
        ++result.added;
    }
    return result;
}

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
    const auto lights = globals::features::nativeLLF.GetLightFrame(frame);
    ImGui::Text("Captured current-frame LLF lights: %zu", lights ? lights->lights.size() : size_t{0});
    bool add = contributeUnmatched.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Contribute unmatched LLF lights", &add))
        contributeUnmatched.store(add, std::memory_order_relaxed);
    float gain = contribution.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("NativeLLF contribution", &gain, 0.0f, 4.0f, "%.2fx"))
        contribution.store(gain, std::memory_order_relaxed);
    ImGui::TextDisabled("Scales unmatched LLF emitter radiance only; prepared lighting remains unchanged.");
    float units = matchDistanceUnits.load(std::memory_order_relaxed);
    if (ImGui::SliderFloat("Clustering distance (Skyrim units)", &units, 0.0f, 512.0f, "%.0f units"))
        matchDistanceUnits.store(units, std::memory_order_relaxed);
    ImGui::TextDisabled("Prepared lights remain first and preserve their original light properties.");
    ImGui::TextDisabled("Nearby LLF lights are folded into their prepared matches, never added twice.");
    const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
    if (scene && globals::game::graphicsState) {
        const auto merged = BuildUnifiedLights(scene->cell->nativeLights, frame);
        ImGui::Text("Prepared %u | matched LLF %u | added LLF %u | unified %u/%u",
            merged.prepared, merged.matched, merged.added, merged.count, kMaxUnifiedLights);
        ImGui::Text("LLF clustered %u | omitted %u | overflow %u",
            merged.clusteredLive, merged.omitted, merged.overflow);
    }
    ImGui::TextDisabled("LLF-only lights participate in DXR shadows against prepared TLAS geometry.");
    ImGui::TextDisabled("Moving objects require their geometry in the TLAS for fully RT-cast shadows.");
    ImGui::TextDisabled("Up to 64 unified lights per native dispatch (prepared-first, then LLF).");
}
void NativeSCMFeature::DrawSettings()
{
    DrawOwnership("Enable Native SCM", enabled, IsActive());
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto casters = ShadowCasterManager::GetNativeCasterFrame(frame);
    ImGui::Text("Tracked current-frame casters: %zu", casters ? casters->casters.size() : size_t{0});
    ImGui::Text("Native TLAS-covered static: %u | awaiting rigid transfer: %u",
        casters ? casters->preparedStaticRetired : 0u,
        casters ? casters->uncoveredRigid : 0u);
    ImGui::Text("Unconverted movers: %u | skinned: %u | fallback raster: %u",
        casters ? casters->moving : 0u,
        casters ? casters->skinned : 0u,
        casters ? casters->rasterFallback : 0u);
    ImGui::TextDisabled("Prepared static casters belong to native TLAS; remaining casters use SCM tracking.");
    ImGui::TextDisabled("Matched LLF lights: raster dynamic caster depth only; prepared geometry: native DXR.");
    ImGui::TextDisabled("Uncovered rigid CPU-backed casters may enter the native DX12 geometry mailbox.");
    ImGui::TextDisabled("DX12 owns BLAS/TLAS and RayQuery; moving and skinned casters still need updates.");
}
void NativePBRFeature::LoadSettings(json& j) { enabled.store(j.value("Enabled", true), std::memory_order_relaxed); }
void NativeLLFFeature::LoadSettings(json& j)
{
    enabled.store(j.value("Enabled", true), std::memory_order_relaxed);
    contributeUnmatched.store(j.value("ContributeUnmatched", true), std::memory_order_relaxed);
    const float value = j.value("MatchDistanceUnits", 64.0f);
    matchDistanceUnits.store(std::isfinite(value) ? std::clamp(value, 0.0f, 512.0f) : 64.0f,
        std::memory_order_relaxed);
    const float strength = j.value("Contribution", 1.0f);
    contribution.store(std::isfinite(strength) ? std::clamp(strength, 0.0f, 4.0f) : 1.0f,
        std::memory_order_relaxed);
}
void NativeSCMFeature::LoadSettings(json& j) { enabled.store(j.value("Enabled", true), std::memory_order_relaxed); }
void NativePBRFeature::SaveSettings(json& j) { j = { { "Enabled", enabled.load(std::memory_order_relaxed) } }; }
void NativeLLFFeature::SaveSettings(json& j)
{
    j = { { "Enabled", enabled.load(std::memory_order_relaxed) },
        { "ContributeUnmatched", contributeUnmatched.load(std::memory_order_relaxed) },
        { "MatchDistanceUnits", matchDistanceUnits.load(std::memory_order_relaxed) },
        { "Contribution", contribution.load(std::memory_order_relaxed) } };
}
void NativeSCMFeature::SaveSettings(json& j) { j = { { "Enabled", enabled.load(std::memory_order_relaxed) } }; }
json NativePBRFeature::GetDiagnostics()
{
    return { { "selected", enabled.load() }, { "native", IsActive() },
        { "recipeRevision", globals::features::truePBR.nativeRegistry.Revision() } };
}
json NativeLLFFeature::GetDiagnostics()
{
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto lights = globals::features::nativeLLF.GetLightFrame(frame);
    json report{ { "selected", enabled.load() }, { "native", IsActive() },
        { "contributeUnmatched", contributeUnmatched.load() },
        { "matchDistanceUnits", matchDistanceUnits.load() },
        { "contribution", contribution.load() },
        { "observedLights", lights ? lights->lights.size() : size_t{0} } };
    const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
    if (scene && globals::game::graphicsState) {
        const auto merged = BuildUnifiedLights(scene->cell->nativeLights, frame);
        report["preparedLights"] = merged.prepared;
        report["unifiedLights"] = merged.count;
        report["matchedLLFLights"] = merged.matched;
        report["addedLLFLights"] = merged.added;
        report["rtShadowEligibleLLFLights"] = merged.added;
        report["clusteredLLFLights"] = merged.clusteredLive;
        report["omittedLLFLights"] = merged.omitted;
        report["overflowLights"] = merged.overflow;
    }
    return report;
}
json NativeSCMFeature::GetDiagnostics()
{
    const auto frame = globals::game::graphicsState ? globals::game::graphicsState->GetFrameCount() : 0u;
    const auto casters = ShadowCasterManager::GetNativeCasterFrame(frame);
    return { { "selected", enabled.load() }, { "native", IsActive() },
        { "observedCasters", casters ? casters->casters.size() : size_t{0} },
        { "retiredPreparedStatic", casters ? casters->preparedStaticRetired : 0u },
        { "uncoveredRigid", casters ? casters->uncoveredRigid : 0u },
        { "movingNotConverted", casters ? casters->moving : 0u },
        { "skinnedNotConverted", casters ? casters->skinned : 0u },
        { "rasterFallback", casters ? casters->rasterFallback : 0u },
        { "evaluationOwner", "DX12 RayQuery only for actually published TLAS casters" } };
}
