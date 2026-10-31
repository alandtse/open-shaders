#pragma once

#include <nlohmann/json.hpp>

namespace PresetPreferences
{
	/** @brief Restores OS-owned preset preferences that CS does not serialize. */
	nlohmann::json Restore(nlohmann::json state);
	/** @brief Persists OS-owned preferences separately without modifying preset payloads. */
	bool Save(const nlohmann::json& state);
}
