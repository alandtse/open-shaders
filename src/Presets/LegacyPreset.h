#pragma once

#include "PresetState.h"
#include "Utils/SettingsPatch.h"

#include <map>
#include <span>
#include <unordered_map>
#include <utility>

namespace LegacyPreset
{
	using FeatureNames = std::pair<std::string, std::string>;

	struct NormalizedPayload
	{
		nlohmann::json values;
		std::vector<std::string> retiredKeys;
	};

	/** @brief Imports recognized legacy keys without rejecting supported values beside retired fields. */
	inline NormalizedPayload NormalizePayload(const nlohmann::json& values, const nlohmann::json& schema)
	{
		NormalizedPayload result{ Util::Settings::SelectSettings(values, schema), {} };
		Util::Settings::CollectUnknownSettingKeys(values, result.values, "", result.retiredKeys);
		return result;
	}

	/** @brief Keeps all legacy global layers before feature-specific layers in automatic preset order. */
	inline std::string BuildPackId(std::string_view safeModName, bool global)
	{
		return std::string(global ? "Legacy (Global) - " : "Legacy - ") + std::string(safeModName);
	}

	/** @brief Converts supported legacy boot preferences into upstream manifest flags. */
	inline std::unordered_map<std::string, bool> TranslateBoot(const nlohmann::json& values,
		std::span<const FeatureNames> features)
	{
		std::unordered_map<std::string, bool> result;
		const auto boot = values.find("Disable at Boot");
		if (boot == values.end() || !boot->is_object())
			return result;
		for (const auto& [shortName, _] : features)
			if (const auto disabled = boot->find(shortName); disabled != boot->end() && disabled->is_boolean())
				result[shortName] = disabled->get<bool>();
		return result;
	}

	/** @brief Splits a legacy global or feature document into upstream Baseline payloads. */
	inline std::map<std::string, nlohmann::json> Translate(const nlohmann::json& values,
		std::string_view featureName, std::span<const FeatureNames> features)
	{
		std::map<std::string, nlohmann::json> result;
		if (!values.is_object())
			return result;
		for (const auto& [shortName, name] : features) {
			const auto source = featureName.empty() ? values.find(name) : values.end();
			const auto* payload = featureName == shortName ? &values :
			                      source != values.end()   ? &*source :
			                                                 nullptr;
			if (payload && payload->is_object() && !payload->empty())
				result[shortName] = *payload;
		}
		return result;
	}

	/** @brief Imports companion edits as personal settings above the preset layer. */
	inline void ImportPersonalEdits(nlohmann::json& settings, const nlohmann::json& edits,
		std::span<const FeatureNames> features)
	{
		if (!edits.is_object())
			return;
		for (const auto& [_, name] : features) {
			const auto values = edits.find(name);
			if (values == edits.end() || !values->is_object() || values->empty())
				continue;
			auto& personal = settings[name];
			if (!personal.is_object())
				personal = nlohmann::json::object();
			personal.update(*values, true);
			PresetState::MarkNormalizedFeatureEdits(settings["Presets"], name);
		}
	}
}
