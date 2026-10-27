#include "SettingsPatch.h"

namespace SettingsBenchmark
{
	nlohmann::json MakePatch(const nlohmann::json& original, const nlohmann::json& modified)
	{
		if (!original.is_object() || !modified.is_object())
			return original == modified ? nlohmann::json::object() : modified;

		auto patch = nlohmann::json::object();
		for (const auto& [key, value] : modified.items()) {
			const auto it = original.find(key);
			if (it == original.end())
				patch[key] = value;
			else if (*it != value)
				patch[key] = it->is_object() && value.is_object() ? MakePatch(*it, value) : value;
		}
		return patch;
	}
}
