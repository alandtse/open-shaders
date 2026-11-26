#pragma once

#include "Feature.h"
#include "DX12ModeProfiles.h"

#include <atomic>

/** @brief Session-wide opt-in for DX12 Resources and future native features. */
struct DX12Control final : Feature
{
	enum class Mode : int
	{
		ShadowOffload = 0,
		RasterComparison = 1,
		FullShadow = 2
	};

	struct Settings
	{
		bool Enabled = false;
		int Mode = int(DX12Control::Mode::ShadowOffload);
	};

	std::string GetName() override { return "DX12"; }
	std::string GetShortName() override { return "DX12"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	// The master checkbox is the only DX12 activation control; do not offer a
	// separate Disable-at-Boot switch that can silently override it.
	bool IsAlwaysEnabled() const override { return true; }
	bool SupportsVR() override { return false; }
	/** @brief Whether native work may be submitted this frame. */
	bool IsActive() const { return loaded && bootActive.load(std::memory_order_acquire); }
	Mode GetSelectedMode() const { return selectedMode.load(std::memory_order_relaxed); }
	bool IsShadowOffloadMode() const { return GetSelectedMode() != Mode::RasterComparison; }
	bool IsFullShadowMode() const { return GetSelectedMode() == Mode::FullShadow; }
	/** @brief Adopt the old Native Scene toggle when loading a pre-mode-selection configuration. */
	void MigrateLegacyFullShadowMode(bool enabled);
	bool UseSpatialReconstruction() const { return IsFullShadowMode() && GetRayTracedSpatialReconstruction(); }
	uint32_t GetReconstructionGridSize() const { return IsFullShadowMode() ? GetRayTracedGridSize() : 3u; }
	float GetShadowStrength() const { return IsFullShadowMode() ? GetRayTracedStrength() : 1.0f; }
	float GetShadowBlend() const { return IsFullShadowMode() ? GetRayTracedBlend() : kOffloadingDefaultBlend; }
	bool GetRayTracedSpatialReconstruction() const { return rayTracedComposite.spatialReconstruction.load(std::memory_order_relaxed); }
	uint32_t GetRayTracedGridSize() const { return rayTracedComposite.reconstructionGrid.load(std::memory_order_relaxed); }
	float GetRayTracedStrength() const { return rayTracedComposite.shadowStrength.load(std::memory_order_relaxed); }
	float GetRayTracedBlend() const { return rayTracedComposite.shadowBlend.load(std::memory_order_relaxed); }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void OnRuntimeDisabled() override;

private:
	std::atomic<bool> bootActive{ false };
	bool bootCaptured = false;
	std::atomic<Mode> selectedMode{ Mode::ShadowOffload };
	bool legacyModeConfig = false;
	struct CompositeProfile
	{
		explicit CompositeProfile(float blend) : shadowBlend(blend) {}
		std::atomic<bool> spatialReconstruction{ true };
		std::atomic<uint32_t> reconstructionGrid{ 5 };
		std::atomic<float> shadowStrength{ 1.0f };
		std::atomic<float> shadowBlend;
	};
	static constexpr float kOffloadingDefaultBlend = 0.40f;
	static constexpr float kRayTracedDefaultBlend = 0.55f;
	CompositeProfile rayTracedComposite{ kRayTracedDefaultBlend };
	static void LoadComposite(CompositeProfile& profile, const json& input, float defaultBlend);
	static json SaveComposite(const CompositeProfile& profile);
	void DrawCompositeDev();
	Settings settings;
};
