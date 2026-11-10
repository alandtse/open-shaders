#pragma once

#include "Feature.h"
#include <atomic>
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
    std::atomic<bool> enabled{ true };
    std::string GetName() override { return "NativeLLF"; }
    std::string GetShortName() override { return "NativeLLF"; }
    std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
    bool IsCore() const override { return true; }
    bool IsActive() const;
    void DrawSettings() override;
    void LoadSettings(json&) override;
    void SaveSettings(json&) override;
    void RestoreDefaultSettings() override { enabled = true; }
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
