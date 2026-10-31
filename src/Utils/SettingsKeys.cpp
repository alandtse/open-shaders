#include "Utils/SettingsPatch.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>

namespace
{
	void OverlaySettingsObject(json& a_defaults, const json& a_userSettings, bool a_preserveAdditionalKeys);

	bool IsCompatibleNumber(const json& a_default, const json& a_user)
	{
		if (!a_user.is_number())
			return false;

		if (a_default.is_number_float()) {
			const double value = a_user.get<double>();
			return std::isfinite(value) && value >= -FLT_MAX && value <= FLT_MAX;
		}

		if (a_default.is_number_unsigned()) {
			if (a_user.is_number_unsigned())
				return a_user.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max();
			if (a_user.is_number_integer()) {
				const auto value = a_user.get<std::int64_t>();
				return value >= 0 && static_cast<std::uint64_t>(value) <= std::numeric_limits<std::uint32_t>::max();
			}

			const double value = a_user.get<double>();
			return std::isfinite(value) && std::trunc(value) == value && value >= 0.0 &&
			       value <= std::numeric_limits<std::uint32_t>::max();
		}

		if (a_user.is_number_unsigned())
			return a_user.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
		if (a_user.is_number_integer()) {
			const auto value = a_user.get<std::int64_t>();
			return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
		}

		const double value = a_user.get<double>();
		return std::isfinite(value) && std::trunc(value) == value &&
		       value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
	}

	bool TryOverlayValue(json& a_default, const json& a_user);

	bool IsIntegerArray(const json& a_default)
	{
		return a_default.is_array() && !a_default.empty() &&
		       std::ranges::all_of(a_default, [](const json& value) {
				   return value.is_number_integer() || value.is_number_unsigned();
			   });
	}

	bool TryOverlayArray(json& a_default, const json& a_user)
	{
		if (!a_user.is_array())
			return false;
		if (a_default.empty()) {
			a_default = a_user;
			return true;
		}

		json filtered = json::array();
		if (a_user.size() == a_default.size()) {
			for (std::size_t index = 0; index < a_user.size(); ++index) {
				json value = a_default[index];
				if (!TryOverlayValue(value, a_user[index]))
					return false;
				filtered.push_back(std::move(value));
			}
		} else {
			if (!IsIntegerArray(a_default))
				return false;
			for (const auto& userValue : a_user) {
				json value = a_default.front();
				if (!TryOverlayValue(value, userValue))
					return false;
				filtered.push_back(std::move(value));
			}
		}

		a_default = std::move(filtered);
		return true;
	}

	bool TryOverlayValue(json& a_default, const json& a_user)
	{
		if (a_default.is_object()) {
			if (!a_user.is_object())
				return false;
			OverlaySettingsObject(a_default, a_user, true);
			return true;
		}
		if (a_default.is_array()) {
			return TryOverlayArray(a_default, a_user);
		}
		if (a_default.is_number()) {
			if (!IsCompatibleNumber(a_default, a_user))
				return false;
			if (a_default.is_number_float())
				a_default = a_user.get<float>();
			else if (a_default.is_number_unsigned())
				a_default = a_user.get<std::uint32_t>();
			else
				a_default = a_user.get<std::int32_t>();
			return true;
		}
		if (a_default.type() != a_user.type())
			return false;

		a_default = a_user;
		return true;
	}

	void OverlaySettingsObject(json& a_defaults, const json& a_userSettings, bool a_preserveAdditionalKeys)
	{
		for (const auto& [key, userValue] : a_userSettings.items()) {
			auto defaultIt = a_defaults.find(key);
			if (defaultIt != a_defaults.end()) {
				TryOverlayValue(*defaultIt, userValue);
			} else if (a_preserveAdditionalKeys) {
				a_defaults[key] = userValue;
			}
		}
	}
}

// Separate translation unit from SettingsPatch.cpp: these helpers use pure
// nlohmann::json recursion with no Feature dependency, so they can compile
// (and be unit-tested) without the engine headers ApplyPatch needs.
namespace Util::Settings
{
	OverwriteValue ResolveOverwriteValue(const json* global, const json* globalUser, const json* feature, const json* featureUser)
	{
		if (feature)
			return { feature, featureUser };
		return global ? OverwriteValue{ global, globalUser } : OverwriteValue{};
	}

	void EraseMetadataKeys(json& settings)
	{
		if (!settings.is_object())
			return;
		for (auto it = settings.begin(); it != settings.end();)
			it = it.key().starts_with('_') ? settings.erase(it) : std::next(it);
	}

	void MergeSettings(json& target, const json& overlay)
	{
		if (!target.is_object() || !overlay.is_object()) {
			target = overlay;
			return;
		}
		for (const auto& [key, value] : overlay.items()) {
			if (const auto found = target.find(key); found != target.end())
				MergeSettings(*found, value);
			else
				target[key] = value;
		}
	}

	bool IsCompatibleSettingValue(const json& known, const json& value)
	{
		if (known.is_null())
			return true;
		if (known.is_number())
			return value.is_boolean() || IsCompatibleNumber(known, value);
		if (known.is_array()) {
			if (!value.is_array())
				return false;
			if (known.empty())
				return true;
			const bool sameSize = known.size() == value.size();
			if (!sameSize && !IsIntegerArray(known))
				return false;
			for (size_t index = 0; index < value.size(); ++index)
				if (!IsCompatibleSettingValue(known[sameSize ? index : 0], value[index]))
					return false;
		}
		return known.type() == value.type();
	}

	void EraseMismatchedSettings(json& incoming, const json& known, const std::string& prefix, std::vector<std::string>& erasedKeys)
	{
		if (!incoming.is_object() || !known.is_object())
			return;
		for (auto it = incoming.begin(); it != incoming.end();) {
			const auto found = known.find(it.key());
			if (found == known.end()) {
				++it;
				continue;
			}
			const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
			if (!IsCompatibleSettingValue(*found, it.value())) {
				erasedKeys.push_back(path);
				it = incoming.erase(it);
				continue;
			}
			EraseMismatchedSettings(it.value(), *found, path, erasedKeys);
			++it;
		}
	}

	json SelectSettingPaths(const json& values, const std::vector<std::string>& paths)
	{
		const std::set<std::string> selected(paths.begin(), paths.end());
		const auto visit = [&](auto&& self, const json& node, const json::json_pointer& parent) -> json {
			json result = json::object();
			if (!node.is_object())
				return result;
			for (const auto& [key, value] : node.items()) {
				if (key.starts_with('_'))
					continue;
				const auto path = parent / key;
				if (value.is_object()) {
					auto nested = self(self, value, path);
					if (!nested.empty())
						result[key] = std::move(nested);
				} else if (selected.contains(path.to_string())) {
					result[key] = value;
				}
			}
			return result;
		};
		return visit(visit, values, json::json_pointer{});
	}

	json SelectSettings(const json& values, const json& mask)
	{
		json selected = json::object();
		if (!values.is_object() || !mask.is_object())
			return selected;
		for (const auto& [key, masked] : mask.items()) {
			const auto value = values.find(key);
			if (value == values.end())
				continue;
			if (masked.is_object() && value->is_object()) {
				auto nested = SelectSettings(*value, masked);
				if (!nested.empty())
					selected[key] = std::move(nested);
			} else {
				selected[key] = *value;
			}
		}
		return selected;
	}

	void RestoreSettings(json& target, const json& source, const json& mask)
	{
		if (!target.is_object() || !mask.is_object())
			return;
		for (const auto& [key, masked] : mask.items()) {
			const auto value = source.is_object() ? source.find(key) : source.end();
			if (masked.is_object() && target.contains(key) && target[key].is_object()) {
				RestoreSettings(target[key], value != source.end() ? *value : json::object(), masked);
				if (target[key].empty() && value == source.end())
					target.erase(key);
			} else if (value != source.end()) {
				target[key] = *value;
			} else {
				target.erase(key);
			}
		}
	}

	void OverlayRecognizedRootSettings(json& a_defaults, const json& a_userSettings)
	{
		if (!a_defaults.is_object() || !a_userSettings.is_object())
			return;

		OverlaySettingsObject(a_defaults, a_userSettings, false);
	}

	void OverlayInputBinding(json& a_binding, const json& a_userBinding)
	{
		const json unsignedValue = std::uint32_t{ 0 };
		if (a_userBinding.is_array()) {
			json values = json::array();
			for (const auto& value : a_userBinding) {
				if (!IsCompatibleNumber(unsignedValue, value))
					return;
				values.push_back(value.get<std::uint32_t>());
			}
			a_binding = std::move(values);
		} else if (IsCompatibleNumber(unsignedValue, a_userBinding)) {
			a_binding = a_userBinding.get<std::uint32_t>();
		}
	}

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

	bool EraseSettingPath(json& document, const json::json_pointer& setting)
	{
		if (setting.empty() || !document.contains(setting))
			return false;
		for (auto parent = setting; !parent.empty(); parent = parent.parent_pointer())
			if (!document.at(parent.parent_pointer()).is_object() || parent.back().starts_with('_'))
				return false;
		auto pointer = setting;
		while (!pointer.empty() && document.contains(pointer)) {
			auto& parent = document.at(pointer.parent_pointer());
			parent.erase(pointer.back());
			if (!parent.empty())
				return true;
			pointer = pointer.parent_pointer();
		}
		return true;
	}

	json BuildUserOverride(const json& current, const json& baseline, const json& savedKeys)
	{
		auto edits = BuildUserOverride(current, baseline);
		MergeSettings(edits, SelectSettings(current, savedKeys));
		return edits;
	}

	void CollectUnknownSettingKeys(const json& a_incoming, const json& a_known,
		const std::string& a_prefix, std::vector<std::string>& a_out)
	{
		if (!a_incoming.is_object())
			return;
		for (auto it = a_incoming.begin(); it != a_incoming.end(); ++it) {
			if (it.key().starts_with('_'))
				continue;
			const std::string path = a_prefix.empty() ? it.key() : a_prefix + "." + it.key();
			if (!a_known.is_object() || !a_known.contains(it.key()))
				a_out.push_back(path);
			else if (!IsCompatibleSettingValue(a_known[it.key()], it.value()))
				a_out.push_back(path + " (wrong type)");
			else if (it.value().is_object())
				CollectUnknownSettingKeys(it.value(), a_known[it.key()], path, a_out);
		}
	}
}
