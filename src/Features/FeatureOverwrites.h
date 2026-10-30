#pragma once

#include "Feature.h"

struct FeatureOverwrites : Feature
{
	std::string GetName() override { return "Feature Overwrites"; }
	std::string GetDisplayName() override { return T("feature.feature_overwrites.name", "Feature Overrides"); }
	std::string GetShortName() override { return "FeatureOverwrites"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool SupportsVR() override { return true; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool UsesMainSettings() const override { return false; }
	bool HasRestoreDefaults() const override { return false; }

	/** @brief Describes management of mod-provided feature settings. */
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	/** @brief Draws applied override and Baseline files with their shared settings editor. */
	void DrawSettings() override;
	/** @brief Exposes the same file management operations to devbench. */
	void RegisterUxActions() override;
};
