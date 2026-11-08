#pragma once

#include "SceneSettingsCatalog.generated.h"

#include <nlohmann/json.hpp>
#include <optional>

struct Feature;

namespace Util::Settings
{
	/**
	 * @brief Reads a catalogued setting from State's captured code defaults.
	 * @return The default, or nullopt when the feature has no default for that setting.
	 * @note Main/render thread only; reading defaults never changes the live feature.
	 */
	std::optional<nlohmann::json> GetSettingDefault(Feature& a_feature, const SceneSettingsCatalog::SettingMetadata& a_setting);

	/**
	 * @brief Restores one catalogued setting to its default, leaving the feature's other settings as they are.
	 * @return False (nothing applied) when the setting has no resolvable default.
	 * @note Main/render thread only.
	 */
	bool ResetSettingToDefault(Feature& a_feature, const SceneSettingsCatalog::SettingMetadata& a_setting);
}
