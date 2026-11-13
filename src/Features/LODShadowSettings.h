#pragma once

#include <nlohmann/json.hpp>

struct LODShadowSettings
{
	bool Enabled = false;
	float UpdateBudgetMs = 0.1f;
	float DepthBias = 8.0f;
};

inline void to_json(nlohmann::json& a_json, const LODShadowSettings& a_settings)
{
	a_json = {
		{ "EnableLODShadows", a_settings.Enabled },
		{ "UpdateBudgetMs", a_settings.UpdateBudgetMs },
		{ "DepthBias", a_settings.DepthBias }
	};
}

inline void from_json(const nlohmann::json& a_json, LODShadowSettings& a_settings)
{
	constexpr LODShadowSettings defaults;
	a_settings.Enabled = a_json.value("EnableLODShadows", defaults.Enabled);
	a_settings.UpdateBudgetMs = a_json.value("UpdateBudgetMs", defaults.UpdateBudgetMs);
	a_settings.DepthBias = a_json.value("DepthBias", defaults.DepthBias);
}
