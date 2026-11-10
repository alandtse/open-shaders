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
#include <BS_thread_pool.hpp>

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

namespace
{
    NativeLLFFeature::UnifiedLights ComputeNativeUnifiedLights(
        std::span<const DX12Native::PreparedPointLight> prepared,
        const NativeLLFFeature::LightFrame* published,
        bool active, float units, float gain, bool contribute);
    std::shared_ptr<const DX12Native::NativeSceneLightBake> BuildNativeSceneLightBake(
        const NativeLLFFeature::UnifiedLights& unified, uint32_t frame,
        const std::shared_ptr<const DX12Native::PreparedCell>& baseline,
        const std::shared_ptr<const DX12Native::NativeSceneLightBake>& previous,
        float matchUnits, float gain);
}

void NativeLLFFeature::ClearCapturedLights()
{
    {
        std::scoped_lock lock(lightMutex);
        lightFrame.reset();
    }
    // Drop all stale scene/frame generations without waiting for workers.
    workerUnified.store({}, std::memory_order_release);
    fallbackUnified.store({}, std::memory_order_release);
    bakeEpoch.fetch_add(1, std::memory_order_acq_rel);
    sceneBake.store({}, std::memory_order_release);
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
    {
        std::scoped_lock lock(lightMutex);
        // A loading transition may have started while the new snapshot was built.
        // Never republish the old cell after its clear operation.
        if (transitionOpen.load(std::memory_order_acquire)) {
            lightFrame.reset();
            return;
        }
        lightFrame = generation;
    }
    // NativeLLF CPU work only: schedule prepared-first matching after owning
    // the live snapshot. On late completion, readers use the exact synchronous
    // fallback rather than a stale/partial native light generation.
    const auto pending = workerUnified.load(std::memory_order_acquire);
    if (pending && !pending->done.load(std::memory_order_acquire)) return;
    const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
    if (!scene || !scene->cell || !IsActive()) return;
    try {
        auto work = std::make_shared<UnifiedWork>();
        work->source = generation;
        work->prepared = scene->cell;
        work->previousBake = scene->bakedLights;
        work->bakeEpoch = bakeEpoch.load(std::memory_order_acquire);
        work->matchUnits = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
        work->gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
        work->contribute = contributeUnmatched.load(std::memory_order_relaxed) && work->gain > 0.0f;
        static BS::thread_pool<> pool{ 1 };
        pool.detach_task([work] {
            try {
                work->result = ComputeNativeUnifiedLights(
                    work->prepared->nativeLights, work->source.get(), true,
                    work->matchUnits, work->gain, work->contribute);
                work->baked = BuildNativeSceneLightBake(
                    work->result, work->source->frame, work->prepared,
                    work->previousBake, work->matchUnits, work->gain);
                work->valid = true;
            } catch (...) { /* Never consume incomplete output. */ }
            work->done.store(true, std::memory_order_release);
        });
        workerUnified.store(std::move(work), std::memory_order_release);
        workerQueued.fetch_add(1, std::memory_order_relaxed);
    } catch (...) { /* Preserve the exact legacy-compatible native fallback. */ }
}
std::shared_ptr<const DX12Native::NativeSceneLightBake> NativeLLFFeature::GetSceneLightBake(
    const std::shared_ptr<const DX12Native::PreparedCell>& baseline) const
{
    if (!baseline || !IsActive() ||
        !bakeUnmatchedIntoScene.load(std::memory_order_relaxed) ||
        !contributeUnmatched.load(std::memory_order_relaxed)) return {};
    const auto gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
    const auto units = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
    if (gain <= 0.0f) return {};
    // Publish only the worker's fully completed generation, exactly scoped to
    // this baseline and reset epoch. No engine pointers or GPU waits involved.
    const auto work = workerUnified.load(std::memory_order_acquire);
    if (work && work->done.load(std::memory_order_acquire) && work->valid &&
        work->baked && work->prepared == baseline &&
        work->bakeEpoch == bakeEpoch.load(std::memory_order_acquire)) {
        auto current = sceneBake.load(std::memory_order_acquire);
        if ((!current || current->generationFrame < work->baked->generationFrame) &&
            sceneBake.compare_exchange_strong(current, work->baked,
                std::memory_order_acq_rel, std::memory_order_acquire))
            bakedPublications.fetch_add(1, std::memory_order_relaxed);
    }
    auto baked = sceneBake.load(std::memory_order_acquire);
    return baked && baked->baseline == baseline &&
        baked->matchUnits == units && baked->contributionGain == gain ? baked : nullptr;
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


namespace
{
NativeLLFFeature::UnifiedLights ComputeNativeUnifiedLights(
    std::span<const DX12Native::PreparedPointLight> prepared,
    const NativeLLFFeature::LightFrame* published,
    bool active, float units, float gain, bool contribute)
{
    NativeLLFFeature::UnifiedLights result{};
    result.prepared = static_cast<uint32_t>(prepared.size());
    result.count = static_cast<uint32_t>(std::min(prepared.size(), result.lights.size()));
    std::copy_n(prepared.begin(), result.count, result.lights.begin());
    result.overflow = result.prepared - result.count;
    // No live LLF data must NEVER erase the authoritative prepared lighting.
    if (!active || !published) return result;
    result.observed = static_cast<uint32_t>(published->lights.size());
    const float radiusSquared = units * units;
    std::array<float, NativeLLFFeature::kMaxUnifiedLights> identityDistances{};
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
// Scene-scope lighting bake: stable, unprepared emitters accumulate in the
// native worker's immutable snapshot. This is not disk persistence: LLF input
// carries no stable placed-form identity with which to validate a disk entry.
std::shared_ptr<const DX12Native::NativeSceneLightBake> BuildNativeSceneLightBake(
    const NativeLLFFeature::UnifiedLights& unified, uint32_t frame,
    const std::shared_ptr<const DX12Native::PreparedCell>& baseline,
    const std::shared_ptr<const DX12Native::NativeSceneLightBake>& previous,
    float matchUnits, float gain)
{
    auto result = std::make_shared<DX12Native::NativeSceneLightBake>();
    result->baseline = baseline;
    result->generationFrame = frame;
    result->matchUnits = matchUnits;
    result->contributionGain = gain;
    constexpr uint32_t kLifetimeFrames = 30;
    const auto distanceSquared = [](const float3& a, const float3& b) {
        const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return x*x + y*y + z*z;
    };
    const auto matchesPrepared = [&](const DX12Native::PreparedPointLight& light) {
        for (uint32_t i = 0; i < std::min(unified.prepared, unified.count); ++i) {
            if (unified.lights[i].supported &&
                distanceSquared(light.position, unified.lights[i].position) <= matchUnits * matchUnits)
                return true;
        }
        return false;
    };
    if (previous && previous->baseline == baseline &&
        previous->matchUnits == matchUnits && previous->contributionGain == gain) {
        for (const auto& entry : previous->candidates) {
            if (frame >= entry.lastSeenFrame &&
                frame - entry.lastSeenFrame <= kLifetimeFrames &&
                !matchesPrepared(entry.light))
                result->candidates.push_back(entry);
        }
    }
    std::vector<bool> touched(result->candidates.size(), false);
    constexpr uint32_t rejectedFlags =
        static_cast<uint32_t>(PointLightFlags::Flags::Particle) |
        static_cast<uint32_t>(PointLightFlags::Flags::Spot) |
        static_cast<uint32_t>(PointLightFlags::Flags::PortalStrict);
    for (uint32_t i = std::min(unified.prepared, unified.count); i < unified.count; ++i) {
        const auto& light = unified.lights[i];
        const auto& source = unified.source[i];
        if (!source.valid || !light.supported ||
            (source.flags & rejectedFlags) != 0 || matchesPrepared(light))
            continue;
        size_t match = result->candidates.size();
        float best = std::numeric_limits<float>::infinity();
        for (size_t j = 0; j < result->candidates.size(); ++j) {
            if (touched[j]) continue;
            const auto& prior = result->candidates[j].light;
            const float tolerance = std::clamp(prior.radius * 0.005f, 1.0f, 8.0f);
            const float d = distanceSquared(light.position, prior.position);
            if (d <= tolerance*tolerance && d < best &&
                std::abs(light.radius - prior.radius) <= std::max(4.0f, prior.radius * 0.03f)) {
                match = j;
                best = d;
            }
        }
        if (match < result->candidates.size()) {
            auto& entry = result->candidates[match];
            touched[match] = true;
            entry.consecutiveObservations = frame > entry.lastSeenFrame &&
                frame - entry.lastSeenFrame <= 4 ?
                std::min(entry.consecutiveObservations + 1u, 1024u) : 1u;
            entry.light = light;
            entry.lastSeenFrame = frame;
        } else if (result->candidates.size() < NativeLLFFeature::kMaxUnifiedLights) {
            result->candidates.push_back({ light, frame, 1u });
            touched.push_back(true);
        }
    }
    return result;
}
} // namespace

NativeLLFFeature::UnifiedLights NativeLLFFeature::BuildUnifiedLights(
    std::span<const DX12Native::PreparedPointLight> prepared, uint32_t frame, bool allowRecent) const
{
    const bool active = IsActive();
    const auto published = active ?
        (allowRecent ? GetRecentLightFrame(frame) : GetLightFrame(frame)) : nullptr;
    const float units = std::clamp(matchDistanceUnits.load(std::memory_order_relaxed), 0.0f, 512.0f);
    const float gain = std::clamp(contribution.load(std::memory_order_relaxed), 0.0f, 4.0f);
    const bool contribute = contributeUnmatched.load(std::memory_order_relaxed) && gain > 0.0f;
    const auto matches = [&](const std::shared_ptr<const UnifiedWork>& work) {
        return work && work->done.load(std::memory_order_acquire) &&
            work->valid && work->source == published && work->prepared &&
            work->prepared->nativeLights.data() == prepared.data() &&
            work->prepared->nativeLights.size() == prepared.size() &&
            work->matchUnits == units && work->gain == gain &&
            work->contribute == contribute;
    };
    if (active && published) {
        if (auto work = workerUnified.load(std::memory_order_acquire); matches(work)) {
            workerHits.fetch_add(1, std::memory_order_relaxed);
            return work->result;
        }
        if (auto cached = fallbackUnified.load(std::memory_order_acquire); matches(cached))
            return cached->result;
    }
    auto result = ComputeNativeUnifiedLights(prepared, published.get(), active, units, gain, contribute);
    if (active && published) {
        auto cached = std::make_shared<UnifiedWork>();
        cached->source = std::move(published);
        // Only the authoritative native scene owns the source span. A
        // matching shared owner is needed for safe future cache reads.
        if (const auto scene = globals::features::dx12NativeScene.GetPublishedScene();
            scene && scene->cell && scene->cell->nativeLights.data() == prepared.data() &&
            scene->cell->nativeLights.size() == prepared.size()) {
            cached->prepared = scene->cell;
            cached->matchUnits = units;
            cached->gain = gain;
            cached->contribute = contribute;
            cached->result = result;
            cached->valid = true;
            cached->done.store(true, std::memory_order_release);
            fallbackUnified.store(std::move(cached), std::memory_order_release);
        }
        fallbackHits.fetch_add(1, std::memory_order_relaxed);
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
    bool sceneBakeEnabled = bakeUnmatchedIntoScene.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Bake stable LLF-only lights into native scene", &sceneBakeEnabled))
        bakeUnmatchedIntoScene.store(sceneBakeEnabled, std::memory_order_relaxed);
    ImGui::TextDisabled("Runtime scene layer; four stable observations and 30-frame expiry.");
    ImGui::TextDisabled("No disk persistence without a verified placed-light identity.");
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
    bakeUnmatchedIntoScene.store(j.value("BakeUnmatchedIntoScene", true), std::memory_order_relaxed);
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
        { "BakeUnmatchedIntoScene", bakeUnmatchedIntoScene.load(std::memory_order_relaxed) },
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
        { "observedLights", lights ? lights->lights.size() : size_t{0} },
        { "sceneBakeEnabled", bakeUnmatchedIntoScene.load(std::memory_order_relaxed) },
        { "sceneBakePublications", bakedPublications.load(std::memory_order_relaxed) },
        { "sceneBakeCandidates", sceneBake.load(std::memory_order_acquire) ?
            sceneBake.load(std::memory_order_acquire)->candidates.size() : size_t{0} },
        { "workerQueued", workerQueued.load(std::memory_order_relaxed) },
        { "workerResultsConsumed", workerHits.load(std::memory_order_relaxed) },
        { "synchronousFallbackCalls", fallbackHits.load(std::memory_order_relaxed) } };
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
