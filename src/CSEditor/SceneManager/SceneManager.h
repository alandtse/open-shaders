#pragma once

#include "Feature.h"
#include "SceneSettingsManager.h"

/** @brief Core feature that hosts the SceneSettingsManager in the feature list. */
struct SceneManager : Feature, SceneSettingsManager
{
	std::string GetName() override { return "Scene Manager"; }
	std::string GetDisplayName() override { return T("feature.scene_manager.name", "Scene Manager"); }
	std::string GetShortName() override { return "SceneManager"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool SupportsVR() override { return true; }
	bool HasRestoreDefaults() const override { return false; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool UsesMainSettings() const override { return false; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Draws the standalone scene authoring page. */
	void DrawSettings() override;
	/** @brief Draws the shared scene transition controls. */
	void DrawTransitions();
	void RegisterUxActions() override;
	void SetupResources() override;
	void DataLoaded() override;
	/** @brief Scene resolution, forwarded to SceneSettingsManager::Update; repeat calls within a frame are no-ops. */
	void Update();
};
