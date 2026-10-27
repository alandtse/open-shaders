#include "Utils/SettingsDefaults.h"

#include "Feature.h"
#include "Utils/SettingsCatalog.h"

#include <unordered_map>

namespace Util::Settings
{
	namespace
	{
		const nlohmann::json& GetFeatureDefaults(Feature& a_feature)
		{
			static std::unordered_map<std::string, nlohmann::json> cache;
			const std::string key(a_feature.GetShortName());
			if (const auto it = cache.find(key); it != cache.end())
				return it->second;

			nlohmann::json current;
			a_feature.SaveSettings(current);
			a_feature.RestoreDefaultSettings();
			nlohmann::json defaults;
			a_feature.SaveSettings(defaults);
			a_feature.LoadSettings(current);
			return cache.emplace(key, std::move(defaults)).first->second;
		}
	}

	std::optional<nlohmann::json> GetSettingDefault(Feature& a_feature, const SceneSettingsCatalog::SettingMetadata& a_setting)
	{
		if (const auto* value = GetCatalogSerializedValue(GetFeatureDefaults(a_feature), a_setting))
			return std::make_optional(*value);
		return std::nullopt;
	}

	bool ResetSettingToDefault(Feature& a_feature, const SceneSettingsCatalog::SettingMetadata& a_setting)
	{
		const auto defaultValue = GetSettingDefault(a_feature, a_setting);
		if (!defaultValue)
			return false;

		nlohmann::json current;
		a_feature.SaveSettings(current);
		auto* value = GetCatalogSerializedValue(current, a_setting);
		if (!value)
			return false;
		*value = *defaultValue;
		a_feature.LoadSettings(current);
		return true;
	}
}
