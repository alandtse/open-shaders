#pragma once

#include <nlohmann/json.hpp>
#include <span>
#include <string_view>

namespace BaselineCompatibility
{
	struct FeatureRule
	{
		std::string_view featureName;
		std::span<const std::string_view> ignoredKeys;
	};

	inline constexpr std::string_view kUpstreamLocalShadowKeys[]{
		"EnableLocalShadows",
		"LocalShadowFilterScale",
		"LocalShadowResolution",
		"LocalShadowSamples",
		"LocalShadowSlots",
	};

	inline constexpr FeatureRule kRules[]{
		{ "LightLimitFix", kUpstreamLocalShadowKeys },
	};

	/** @brief Erases the top-level settings ignored for the named feature. */
	template <const auto& Rules = kRules>
	void EraseIgnoredSettings(std::string_view featureName, nlohmann::json& values)
	{
		for (const auto& rule : Rules) {
			if (rule.featureName != featureName)
				continue;
			for (const auto key : rule.ignoredKeys)
				values.erase(key);
		}
	}
}
