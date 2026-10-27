#pragma once

#include <nlohmann/json.hpp>

namespace SettingsBenchmark
{
	/**
	 * @brief Builds the smallest merge patch that turns `original` into `modified`.
	 *
	 * Objects are compared key by key; any other differing value, including a whole array, is copied.
	 * Keys present only in `original` are not removed, so the result never deletes a setting.
	 * @return An object that is empty when nothing differs.
	 */
	nlohmann::json MakePatch(const nlohmann::json& original, const nlohmann::json& modified);
}
