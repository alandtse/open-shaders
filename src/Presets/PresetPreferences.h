#pragma once

#include <functional>
#include <nlohmann/json.hpp>

namespace PresetPreferences
{
	/** @brief Restores OS-owned preset preferences that CS does not serialize. */
	nlohmann::json Restore(nlohmann::json state);
	/** @brief Saves local metadata and an optional settings write, rolling metadata back on failure. */
	bool Save(const nlohmann::json& state, const std::function<bool()>& saveSettings = {});
}
