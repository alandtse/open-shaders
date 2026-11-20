#pragma once

#include "AccelerationScene.h"
#include "PreparedLightMatching.h"

#include <atomic>
#include <limits>

/** @brief Prepared-light-only DXR scene with deferred publication of completed geometry. */
struct DX12NativeScene final : Feature
{
	struct Settings
	{
		bool Enabled = true;
		DX12::LightTuning lights;
	};

	std::string GetName() override { return "DX12 Native Scene"; }
	std::string GetShortName() override { return "DX12NativeScene"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool SupportsVR() override { return false; }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	bool WantsPreShadowMapsHook() const override { return true; }
	void PreShadowMaps() override;
	void OnSceneTransitionReset(bool opening) override;
	void OnRuntimeDisabled() override;
	bool IsActive() const;
	std::shared_ptr<const DX12::AccelerationSceneReady> GetScene() const { return scene.Ready(); }
	std::shared_ptr<const DX12::ResolvedLights> GetLights() const { return lights.load(std::memory_order_acquire); }
	/** @brief Prepared-reference identities with verified per-light static coverage in the published TLAS. */
	std::shared_ptr<const std::vector<uint32_t>> GetCoveredLightIDs() const { return coveredLightIDs.load(std::memory_order_acquire); }

private:
	void UpdateLights();
	void PublishCoverage(const DX12::Grid& grid);
	DX12::AccelerationScene scene;
	std::atomic<std::shared_ptr<const DX12::ResolvedLights>> lights;
	// Immutable companion lights are re-collected only when grid publications change.
	std::vector<DX12::PreparedLight> preparedCache;
	uint64_t preparedCacheRevision = std::numeric_limits<uint64_t>::max();
	uint32_t preparedCacheScope = 0;
	std::atomic<std::shared_ptr<const std::vector<uint32_t>>> coveredLightIDs;
	Settings settings;
};
