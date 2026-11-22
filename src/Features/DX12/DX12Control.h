#pragma once

#include "Feature.h"

#include <atomic>

/** @brief Session-wide opt-in for DX12 Resources and future native features. */
struct DX12Control final : Feature
{
	struct Settings
	{
		bool Enabled = false;
		int Mode = 0;  // 0: shadow offload, 1: raster comparison
		bool SpatialReconstruction = true;
		uint32_t ReconstructionGridSize = 5;  // Odd, 3x3 through 9x9.
		float ShadowStrength = 1.0f;
		float ShadowBlend = 0.70f;
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
	bool IsShadowOffloadMode() const { return selectedMode.load(std::memory_order_relaxed) == 0; }
	bool UseSpatialReconstruction() const { return spatialReconstruction.load(std::memory_order_relaxed); }
	uint32_t GetReconstructionGridSize() const { return reconstructionGrid.load(std::memory_order_relaxed); }
	float GetShadowStrength() const { return shadowStrength.load(std::memory_order_relaxed); }
	float GetShadowBlend() const { return shadowBlend.load(std::memory_order_relaxed); }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void OnRuntimeDisabled() override;

private:
	std::atomic<bool> bootActive{ false };
	bool bootCaptured = false;
	std::atomic<int> selectedMode{ 0 };
	std::atomic<bool> spatialReconstruction{ true };
	std::atomic<uint32_t> reconstructionGrid{ 5 };
	std::atomic<float> shadowStrength{ 1.0f };
	std::atomic<float> shadowBlend{ 0.70f };
	Settings settings;
};
