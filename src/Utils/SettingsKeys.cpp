#include "Utils/SettingsPatch.h"

#include <algorithm>
#include <cmath>

// Separate translation unit from SettingsPatch.cpp: these helpers use pure
// nlohmann::json recursion with no Feature dependency, so they can compile
// (and be unit-tested) without the engine headers ApplyPatch needs.
namespace Util::Settings
{
	json BuildUserOverride(const json& a_current, const json& a_override)
	{
		json userOverride = json::object();
		if (!a_current.is_object() || !a_override.is_object())
			return userOverride;

		for (const auto& [key, overrideValue] : a_override.items()) {
			if (!a_current.contains(key))
				continue;

			const auto& currentValue = a_current[key];
			if (currentValue.is_object() && overrideValue.is_object()) {
				json nestedOverride = BuildUserOverride(currentValue, overrideValue);
				if (!nestedOverride.empty())
					userOverride[key] = std::move(nestedOverride);
			} else if (currentValue.is_number() && overrideValue.is_number()) {
				double current = currentValue.get<double>();
				double overridden = overrideValue.get<double>();
				double tolerance = std::max(1e-6, std::abs(overridden) * 1e-5);
				if (std::abs(current - overridden) > tolerance)
					userOverride[key] = currentValue;
			} else if (currentValue != overrideValue) {
				userOverride[key] = currentValue;
			}
		}

		return userOverride;
	}

	void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
		const std::string& a_prefix, std::vector<std::string>& a_out)
	{
		if (!a_incoming.is_object())
			return;
		for (auto it = a_incoming.begin(); it != a_incoming.end(); ++it) {
			const std::string path = a_prefix.empty() ? it.key() : a_prefix + "." + it.key();
			if (!a_known.is_object() || !a_known.contains(it.key()))
				a_out.push_back(path);
			else if (it.value().is_object())
				CollectUnknownSettingKeys(it.value(), a_known[it.key()], path, a_out);
		}
	}
}
