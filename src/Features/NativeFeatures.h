#pragma once

#include "Feature.h"
#include "LightLimitFix.h"
#include "Features/DX12NativeScene/PreparedScene.h"
#include <array>
#include <atomic>
#include <span>
#include <memory>
#include <mutex>
#include <vector>
namespace RE { class BSShadowLight; }

/** Additional DX12 features. The original TruePBR/LLF/SCM implementations are
 * unchanged as the legacy baseline and continue to collect Skyrim state.
 * Each native feature takes effect when the DX12 Native Scene backend is
 * selected. Missing outputs do not trigger per-effect legacy fallback.
 */
struct NativePBRFeature final : Feature
{
    std::atomic<bool> enabled{ true };
    std::string GetName() override { return "NativePBR"; }
    std::string GetShortName() override { return "NativePBR"; }
    std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }
    bool IsCore() const override { return true; }
    bool IsActive() const;
    void DrawSettings() override;
    void LoadSettings(json&) override;
    void SaveSettings(json&) override;
    void RestoreDefaultSettings() override { enabled = true; }
    json GetDiagnostics() override;
};

struct NativeLLFFeature final : Feature
{
    // Native light generations belong to NativeLLF, never to the DX11 LLF baseline.
    // The baseline exposes one post-override CPU buffer at its existing update boundary.
    struct Light
    {
        float3 position{}, radiance{};
        float radius = 0.0f, sourceRadius = 0.0f, fadeZone = 0.0f;
        uint32_t flags = 0, shadowSlot = 0;
    };
    struct LightFrame
    {
        uint32_t frame = 0;
        std::vector<Light> lights;
    };
    std::atomic<bool> enabled{ true };
    // Only supplementary LLF emitters are toggled; prepared scene lighting
    // stays authoritative, while live observations remain available to SCM.
    std::atomic<bool> contributeUnmatched{ true };
    std::atomic<float> matchDistanceUnits{ 64.0f };
    static constexpr uint32_t kMaxUnifiedLights = 32;
    struct SourceIdentity
    {
        float3 position{};
        float radius = 0.0f;
        uint32_t flags = 0;
        uint32_t shadowSlot = UINT32_MAX;
        bool valid = false;
    };
    struct UnifiedLights
    {
        std::array<DX12Native::PreparedPointLight, kMaxUnifiedLights> lights{};
        std::array<SourceIdentity, kMaxUnifiedLights> source{};
        uint32_t count = 0, prepared = 0, observed = 0, matched = 0, added = 0;
        uint32_t clusteredLive = 0, omitted = 0, overflow = 0;
    };
    UnifiedLights BuildUnifiedLights(std::span<const DX12Native::PreparedPointLight> prepared,
        uint32_t frame, bool allowRecent = false) const;
    void PublishLights(const LightLimitFix::LightData* data, uint32_t count, const float3& eye, uint32_t frame);
    std::shared_ptr<const LightFrame> GetLightFrame(uint32_t frame) const;
    std::shared_ptr<const LightFrame> GetRecentLightFrame(uint32_t frame) const;
private:
    mutable std::mutex lightMutex;
    std::shared_ptr<const LightFrame> lightFrame;
public:
    std::string GetName() override { return "NativeLLF"; }
    std::string GetShortName() override { return "NativeLLF"; }
    std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
    bool IsCore() const override { return true; }
    bool IsActive() const;
    void DrawSettings() override;
    void LoadSettings(json&) override;
    void SaveSettings(json&) override;
    void RestoreDefaultSettings() override
    {
        enabled = true; contributeUnmatched = true; matchDistanceUnits = 64.0f;
    }
    json GetDiagnostics() override;
};

struct NativeSCMFeature final : Feature
{
    std::atomic<bool> enabled{ true };
    /** Exact LLF/SCM slot identity selected for native ray-shadow evaluation.
     *  This is a per-light ownership decision, never a renderer fallback gate. */
    bool OwnsStaticForLight(const RE::BSShadowLight* light, int32_t slot) const;
    std::string GetName() override { return "NativeSCM"; }
    std::string GetShortName() override { return "NativeSCM"; }
    std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
    bool IsCore() const override { return true; }
    bool IsActive() const;
    void DrawSettings() override;
    void LoadSettings(json&) override;
    void SaveSettings(json&) override;
    void RestoreDefaultSettings() override { enabled = true; }
    json GetDiagnostics() override;
};
