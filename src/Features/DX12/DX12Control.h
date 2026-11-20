#pragma once

#include "Feature.h"

#include <atomic>

/** @brief Session-wide opt-in for the native DX12 scene, shadow and composite pipeline. */
struct DX12Control final : Feature
{
	struct Settings
	{
		bool Enabled = false;
	};

	std::string GetName() override { return "DX12"; }
	std::string GetShortName() override { return "DX12"; }
	std::string_view GetCategory() const override { return FeatureCategories::kDX12; }
	bool IsCore() const override { return true; }
	bool SupportsVR() override { return false; }
	/** @brief Whether native work may be submitted this frame. */
	bool IsActive() const { return loaded && active.load(std::memory_order_acquire); }
	void DrawSettings() override;
	void LoadSettings(json& input) override;
	void SaveSettings(json& output) override;
	void RestoreDefaultSettings() override;
	void OnRuntimeDisabled() override;

private:
	std::atomic<bool> active{ false };
	Settings settings;
};
