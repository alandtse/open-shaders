#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace BaselineCompatibility
{
	/** @brief Drops settings for upstream's local shadow implementation from Light Limit Fix baselines. */
	inline void EraseUpstreamShadowSettings(std::string_view featureName, nlohmann::json& values)
	{
		if (featureName == "LightLimitFix") {
			for (const auto* key : { "EnableLocalShadows", "LocalShadowFilterScale", "LocalShadowResolution", "LocalShadowSamples", "LocalShadowSlots" })
				values.erase(key);
		}
	}
}
