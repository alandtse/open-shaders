#include "Utils/SettingsDefaults.h"

#include "Feature.h"
#include "Globals.h"
#include "State.h"
#include "Utils/SettingsCatalog.h"

namespace Util::Settings
{
	std::optional<nlohmann::json> GetSettingDefault(Feature& a_feature, const SceneSettingsCatalog::SettingMetadata& a_setting)
	{
		if (const auto* defaults = globals::state ? globals::state->GetFeatureDefaults(a_feature.GetName()) : nullptr)
			if (const auto* value = GetCatalogSerializedValue(*defaults, a_setting))
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
