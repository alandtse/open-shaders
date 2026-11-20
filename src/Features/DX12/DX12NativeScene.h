#pragma once

#include "AccelerationScene.h"
#include "PreparedLightMatching.h"

#include <atomic>

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
	void EarlyPrepass() override;
	void OnSceneTransitionReset(bool opening) override;
	void OnRuntimeDisabled() override;
	bool IsActive() const;
	std::shared_ptr<const DX12::AccelerationSceneReady> GetScene() const { return scene.Ready(); }
	std::shared_ptr<const DX12::ResolvedLights> GetLights() const { return lights.load(std::memory_order_acquire); }

private:
	void UpdateLights();
	DX12::AccelerationScene scene;
	std::atomic<std::shared_ptr<const DX12::ResolvedLights>> lights;
	Settings settings;
};
